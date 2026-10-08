#include "obe/net/uring.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "obe/net/socket.hpp"
#include "support/tcp_client.hpp"

// The rings themselves (obe/net/uring.hpp): that entries go in, that
// completions come out with the user_data they went in with and the result
// the matching system call would give, and the things about cancelling and
// waiting that a transport is built on.
//
// Every test skips itself where io_uring cannot be used: an old kernel, a
// container that forbids it. A run in which they were all skipped has tested
// nothing, and says so in its summary. Where the build's own check found
// io_uring working (support/tcp_client.hpp, OBE_UNAVAILABLE), a ring that
// says it cannot be used fails its tests instead: a broken ring must not be
// able to excuse itself.

namespace {

using namespace obe;
using namespace std::chrono_literals;
using net::Uring;

class UringTest : public ::testing::Test {
 protected:
    void SetUp() override {
        if (!Uring::available()) {
            OBE_UNAVAILABLE("the ring");
        }
        int fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
        near = net::Fd(fds[0]);
        far = net::Fd(fds[1]);
    }

    // Every completion that is there, by user_data.
    std::map<std::uint64_t, int> reap(Uring& ring) {
        std::map<std::uint64_t, int> out;
        ring.reap([&out](const io_uring_cqe& cqe) {
            EXPECT_EQ(out.count(cqe.user_data), 0U) << "two completions for " << cqe.user_data;
            out[cqe.user_data] = cqe.res;
        });
        return out;
    }

    // Waits (for at most ten seconds) until `count` completions have been
    // collected.
    std::map<std::uint64_t, int> reap_at_least(Uring& ring, std::size_t count) {
        std::map<std::uint64_t, int> out;
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (out.size() < count && std::chrono::steady_clock::now() < deadline) {
            EXPECT_GE(ring.submit_and_wait(50), 0);
            for (const auto& [user_data, res] : reap(ring)) {
                out[user_data] = res;
            }
        }
        return out;
    }

    net::Fd near;  // a connected pair of sockets
    net::Fd far;
};

using Results = std::map<std::uint64_t, int>;

TEST_F(UringTest, ARingHasAtLeastTheRoomAskedFor) {
    Uring ring(10);
    EXPECT_GE(ring.sq_entries(), 10U);
    EXPECT_EQ(ring.sq_entries() & (ring.sq_entries() - 1), 0U) << "a power of two";
    EXPECT_GE(ring.cq_entries(), ring.sq_entries());
    EXPECT_EQ(ring.unsubmitted(), 0U);
    EXPECT_EQ(ring.ready(), 0U);
    EXPECT_EQ(ring.enters(), 0U);
}

TEST_F(UringTest, AnOperationComesBackWithItsUserData) {
    Uring ring(8);
    io_uring_sqe* sqe = ring.get_sqe();
    ASSERT_NE(sqe, nullptr);
    net::prep_nop(sqe, 0xDEADBEEFCAFEF00DULL);
    EXPECT_EQ(ring.unsubmitted(), 1U);
    EXPECT_EQ(ring.ready(), 0U) << "nothing happens until the kernel is told";
    EXPECT_EQ(ring.submit(), 1);
    EXPECT_EQ(ring.unsubmitted(), 0U);
    EXPECT_EQ(ring.ready(), 1U) << "an operation that cannot wait is done when submit returns";
    EXPECT_EQ(reap(ring), (Results{{0xDEADBEEFCAFEF00DULL, 0}}));
    EXPECT_EQ(ring.ready(), 0U);
    EXPECT_EQ(ring.enters(), 1U);
}

TEST_F(UringTest, SubmittingNothingDoesNotEnterTheKernel) {
    Uring ring(8);
    EXPECT_EQ(ring.submit(), 0);
    EXPECT_EQ(ring.submit_and_wait(0), 0);
    EXPECT_EQ(ring.enters(), 0U);
}

TEST_F(UringTest, ManyOperationsGoInWithOneCall) {
    Uring ring(32);
    for (std::uint64_t i = 1; i <= 20; ++i) {
        io_uring_sqe* sqe = ring.get_sqe();
        ASSERT_NE(sqe, nullptr);
        net::prep_nop(sqe, i);
    }
    EXPECT_EQ(ring.submit(), 20);
    EXPECT_EQ(ring.enters(), 1U) << "twenty operations, one system call";
    const Results results = reap(ring);
    EXPECT_EQ(results.size(), 20U);
    for (std::uint64_t i = 1; i <= 20; ++i) {
        EXPECT_EQ(results.count(i), 1U) << i;
    }
}

TEST_F(UringTest, AFullSubmissionQueueSaysSoAndSubmittingMakesRoom) {
    Uring ring(4);
    const unsigned room = ring.sq_entries();
    std::uint64_t next = 0;
    for (unsigned i = 0; i < room; ++i) {
        io_uring_sqe* sqe = ring.get_sqe();
        ASSERT_NE(sqe, nullptr) << "entry " << i << " of " << room;
        net::prep_nop(sqe, ++next);
    }
    EXPECT_EQ(ring.get_sqe(), nullptr) << "the queue holds " << room << " and gave out more";
    EXPECT_EQ(ring.unsubmitted(), room);
    EXPECT_EQ(ring.submit(), static_cast<int>(room));
    io_uring_sqe* sqe = ring.get_sqe();
    ASSERT_NE(sqe, nullptr) << "submitting did not make room";
    net::prep_nop(sqe, ++next);
    EXPECT_EQ(ring.submit(), 1);
    EXPECT_EQ(reap(ring).size(), room + 1);
}

// An entry is handed out zeroed, whatever the operation that last used its
// slot left in it. Entries are a union of fields for every kind of operation:
// one operation's flags are another's offset.
TEST_F(UringTest, AnEntryIsHandedOutZeroed) {
    Uring ring(4);
    const auto all_zero = [](const io_uring_sqe* sqe) {
        // Byte by byte: the entry is a union with padding, and comparing two
        // of them as objects would compare bytes that mean nothing.
        std::array<unsigned char, sizeof(io_uring_sqe)> bytes{};
        std::memcpy(bytes.data(), sqe, sizeof(io_uring_sqe));
        return std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0; });
    };
    // Twice round the ring, leaving every slot full of ones each time.
    for (unsigned i = 0; i < ring.sq_entries() * 2; ++i) {
        io_uring_sqe* sqe = ring.get_sqe();
        ASSERT_NE(sqe, nullptr);
        ASSERT_TRUE(all_zero(sqe)) << "entry " << i << " was not zeroed";
        std::memset(sqe, 0xFF, sizeof(*sqe));
        // Still an operation the kernel can dispose of: one of a kind that
        // does not exist. It completes with an error and touches nothing.
        sqe->fd = -1;
        sqe->user_data = i;
        ASSERT_EQ(ring.submit(), 1);
        std::size_t done = 0;
        ring.reap([&](const io_uring_cqe& cqe) {
            EXPECT_LT(cqe.res, 0);
            ++done;
        });
        ASSERT_EQ(done, 1U);
    }
}

// More completions than the completion queue holds. The kernel keeps the
// ones that do not fit and hands them over as room is made: none is lost.
TEST_F(UringTest, NoCompletionIsLostWhenTheCompletionQueueOverflows) {
    Uring ring(4);
    const unsigned total = ring.cq_entries() * 3;
    std::vector<int> seen(total, 0);
    unsigned sent = 0;
    unsigned collected = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (collected < total && std::chrono::steady_clock::now() < deadline) {
        // Submit without reaping for as long as the kernel accepts them.
        while (sent < total) {
            io_uring_sqe* sqe = ring.get_sqe();
            if (sqe == nullptr) {
                const int submitted = ring.submit();
                if (submitted <= 0) {
                    break;  // an older kernel, backed up (-EBUSY): reap first
                }
                continue;
            }
            net::prep_nop(sqe, sent++);
        }
        static_cast<void>(ring.submit_and_wait(10));
        ring.reap([&](const io_uring_cqe& cqe) {
            ASSERT_LT(cqe.user_data, total);
            ++seen[cqe.user_data];
            ++collected;
        });
    }
    EXPECT_EQ(collected, total);
    for (unsigned i = 0; i < total; ++i) {
        ASSERT_EQ(seen[i], 1) << "operation " << i;
    }
}

// The same, for a program that never waits: it only ever submits, or asks
// for what is there without waiting, as a loop that polls with a timeout of
// zero does. The completions the kernel is keeping come into the ring only
// during a call that asks for completions, so the ring has to notice that
// some are being kept and ask, although nothing else about the call needs
// it. A ring that does not is left with operations that finished and are
// never heard of.
TEST_F(UringTest, CompletionsTheKernelIsKeepingArriveWithoutAWait) {
    for (const bool only_submit : {true, false}) {
        SCOPED_TRACE(only_submit ? "submit()" : "submit_and_wait(0)");
        Uring ring(4);
        const unsigned total = ring.cq_entries() * 3;
        std::vector<int> seen(total, 0);
        unsigned sent = 0;
        unsigned collected = 0;
        // Nothing here depends on time: every operation completes at once.
        // The bound is on turns, and it is generous.
        for (unsigned turn = 0; turn < total * 4 && collected < total; ++turn) {
            while (sent < total) {
                io_uring_sqe* sqe = ring.get_sqe();
                if (sqe == nullptr) {
                    if (ring.submit() <= 0) {
                        break;  // an older kernel, backed up: reap first
                    }
                    continue;
                }
                net::prep_nop(sqe, sent++);
            }
            const int entered = only_submit ? ring.submit() : ring.submit_and_wait(0);
            ASSERT_GE(entered, 0);
            ring.reap([&](const io_uring_cqe& cqe) {
                ASSERT_LT(cqe.user_data, total);
                ++seen[cqe.user_data];
                ++collected;
            });
        }
        EXPECT_EQ(sent, total);
        EXPECT_EQ(collected, total) << "completions were left with the kernel";
        for (unsigned i = 0; i < total; ++i) {
            ASSERT_EQ(seen[i], 1) << "operation " << i;
        }
        // With nothing being kept, a call with nothing to do is no call.
        const std::uint64_t enters = ring.enters();
        EXPECT_EQ(ring.submit(), 0);
        EXPECT_EQ(ring.submit_and_wait(0), 0);
        EXPECT_EQ(ring.enters(), enters);
    }
}

TEST_F(UringTest, AReceiveCompletesWhenBytesArriveAndNotBefore) {
    Uring ring(8);
    std::array<std::byte, 64> buffer{};
    net::prep_recv(ring.get_sqe(), near.get(), buffer.data(), buffer.size(), 7);
    EXPECT_EQ(ring.submit(), 1);
    EXPECT_EQ(ring.ready(), 0U) << "nothing has been sent yet";

    const auto before = std::chrono::steady_clock::now();
    EXPECT_EQ(ring.submit_and_wait(40), 0) << "a wait that times out is not an error";
    EXPECT_GE(std::chrono::steady_clock::now() - before, 30ms) << "it did not wait";
    EXPECT_EQ(ring.ready(), 0U);

    ASSERT_EQ(::send(far.get(), "hello", 5, 0), 5);
    EXPECT_EQ(reap_at_least(ring, 1), (Results{{7, 5}}));
    EXPECT_EQ(std::memcmp(buffer.data(), "hello", 5), 0);
}

TEST_F(UringTest, AWaitReturnsAtOnceWhenSomethingIsAlreadyThere) {
    Uring ring(8);
    net::prep_nop(ring.get_sqe(), 1);
    ASSERT_EQ(ring.submit(), 1);
    const std::uint64_t enters = ring.enters();
    const auto before = std::chrono::steady_clock::now();
    EXPECT_EQ(ring.submit_and_wait(5'000), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
    EXPECT_EQ(ring.enters(), enters) << "there was nothing to submit and nothing to wait for";
    EXPECT_EQ(reap(ring).size(), 1U);
}

TEST_F(UringTest, ASendGivesTheBytesToTheOtherEnd) {
    Uring ring(8);
    const char text[] = "an order, perhaps";
    net::prep_send(ring.get_sqe(), near.get(), reinterpret_cast<const std::byte*>(text),
                   sizeof(text), MSG_NOSIGNAL, 3);
    EXPECT_EQ(ring.submit(), 1);
    EXPECT_EQ(reap(ring), (Results{{3, static_cast<int>(sizeof(text))}}))
        << "a send to a socket with room is done when submit returns";
    char got[64] = {};
    ASSERT_EQ(::recv(far.get(), got, sizeof(got), 0), static_cast<ssize_t>(sizeof(text)));
    EXPECT_STREQ(got, text);
}

// The flags of a send are the system call's flags. With MSG_DONTWAIT a send
// to a socket that is full fails at once with -EAGAIN; without it, the
// operation waits in the kernel until there is room.
TEST_F(UringTest, ASendCarriesItsFlags) {
    // Fill the socket until it takes no more.
    const std::vector<std::byte> filler(std::size_t{64} * 1024, std::byte{0x5A});
    while (::send(near.get(), filler.data(), filler.size(), MSG_DONTWAIT | MSG_NOSIGNAL) > 0) {
    }
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

    Uring ring(8);
    net::prep_send(ring.get_sqe(), near.get(), filler.data(), filler.size(),
                   MSG_DONTWAIT | MSG_NOSIGNAL, 60);
    ASSERT_EQ(ring.submit(), 1);
    EXPECT_EQ(reap(ring), (Results{{60, -EAGAIN}})) << "MSG_DONTWAIT did not reach the kernel";

    // The same send without the flag waits: nothing completes until the
    // other end makes room by reading.
    net::prep_send(ring.get_sqe(), near.get(), filler.data(), filler.size(), MSG_NOSIGNAL, 61);
    ASSERT_EQ(ring.submit(), 1);
    static_cast<void>(ring.submit_and_wait(30));
    EXPECT_EQ(ring.ready(), 0U) << "a send to a full socket completed without room being made";
    std::vector<std::byte> sink(1 << 20);
    std::size_t drained = 0;
    Results results;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (results.empty() && std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::recv(far.get(), sink.data(), sink.size(), MSG_DONTWAIT);
        drained += n > 0 ? static_cast<std::size_t>(n) : 0;
        static_cast<void>(ring.submit_and_wait(5));
        results = reap(ring);
    }
    ASSERT_EQ(results.size(), 1U);
    EXPECT_GT(results.at(61), 0) << "once there was room, the send went through";
    EXPECT_GT(drained, 0U);
}

TEST_F(UringTest, AReceiveSeesTheOtherEndClose) {
    Uring ring(8);
    std::array<std::byte, 16> buffer{};
    net::prep_recv(ring.get_sqe(), near.get(), buffer.data(), buffer.size(), 9);
    ASSERT_EQ(ring.submit(), 1);
    far.reset();
    EXPECT_EQ(reap_at_least(ring, 1), (Results{{9, 0}})) << "0 bytes: the end of the stream";
}

TEST_F(UringTest, ErrorsComeBackAsNegativeErrnoValues) {
    Uring ring(8);
    std::array<std::byte, 16> buffer{};
    const int not_a_socket = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(not_a_socket, 0);
    net::prep_recv(ring.get_sqe(), not_a_socket, buffer.data(), buffer.size(), 1);
    net::prep_recv(ring.get_sqe(), 987'654, buffer.data(), buffer.size(), 2);
    ASSERT_EQ(ring.submit(), 2);
    const Results results = reap_at_least(ring, 2);
    ::close(not_a_socket);
    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results.at(1), -ENOTSOCK);
    EXPECT_EQ(results.at(2), -EBADF);
}

TEST_F(UringTest, AnAcceptCompletesWithTheNewSocket) {
    Uring ring(8);
    const net::Fd listener = net::listen_tcp("127.0.0.1", 0);
    // A blocking listener: on older kernels an accept through the ring on a
    // non-blocking one does not wait (uring.hpp, prep_accept).
    net::set_blocking(listener.get());
    ASSERT_EQ(::fcntl(listener.get(), F_GETFL) & O_NONBLOCK, 0);
    net::prep_accept(ring.get_sqe(), listener.get(), SOCK_CLOEXEC, 11);
    ASSERT_EQ(ring.submit(), 1);
    EXPECT_EQ(ring.ready(), 0U) << "nobody has connected";

    const net::Fd client = net::connect_tcp("127.0.0.1", net::local_port(listener.get()), false);
    const Results results = reap_at_least(ring, 1);
    ASSERT_EQ(results.size(), 1U);
    ASSERT_GE(results.at(11), 0) << "the result is the accepted socket";
    const net::Fd accepted(results.at(11));
    // It really is the other end of the client's connection.
    ASSERT_EQ(::send(client.get(), "x", 1, MSG_NOSIGNAL), 1);
    char byte = 0;
    for (int i = 0; i < 1'000 && ::recv(accepted.get(), &byte, 1, MSG_DONTWAIT) != 1; ++i) {
        ::usleep(1'000);
    }
    EXPECT_EQ(byte, 'x');
    EXPECT_NE(::fcntl(accepted.get(), F_GETFD) & FD_CLOEXEC, 0) << "the flags were not applied";
}

// Two completions for one cancellation: the cancel's own, and the cancelled
// operation's. A transport that forgets the second frees a buffer the kernel
// has not let go of.
TEST_F(UringTest, CancellingAnOperationYieldsTwoCompletions) {
    Uring ring(8);
    std::array<std::byte, 16> buffer{};
    net::prep_recv(ring.get_sqe(), near.get(), buffer.data(), buffer.size(), 20);
    ASSERT_EQ(ring.submit(), 1);
    net::prep_cancel(ring.get_sqe(), 20, 21);
    const Results results = reap_at_least(ring, 2);
    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results.at(21), 0) << "the cancel found its target";
    EXPECT_EQ(results.at(20), -ECANCELED) << "the receive ended without receiving";

    // Cancelling what is not there says so, and yields one completion.
    net::prep_cancel(ring.get_sqe(), 12'345, 22);
    EXPECT_EQ(reap_at_least(ring, 1), (Results{{22, -ENOENT}}));
}

// Shutting a socket down is the other way to end what is waiting on it.
TEST_F(UringTest, ShuttingASocketDownEndsAReceiveThatIsWaitingOnIt) {
    Uring ring(8);
    std::array<std::byte, 16> buffer{};
    net::prep_recv(ring.get_sqe(), near.get(), buffer.data(), buffer.size(), 30);
    ASSERT_EQ(ring.submit(), 1);
    ASSERT_EQ(::shutdown(near.get(), SHUT_RDWR), 0);
    EXPECT_EQ(reap_at_least(ring, 1), (Results{{30, 0}}));
}

// What a transport must never rely on: closing the descriptor does NOT end
// an operation that is waiting on it. The operation holds the socket open
// and goes on waiting, with its buffer.
TEST_F(UringTest, ClosingADescriptorDoesNotEndWhatIsWaitingOnIt) {
    Uring ring(8);
    std::array<std::byte, 16> buffer{};
    net::prep_recv(ring.get_sqe(), near.get(), buffer.data(), buffer.size(), 40);
    ASSERT_EQ(ring.submit(), 1);
    near.reset();  // closed, as far as this process's descriptor table goes
    static_cast<void>(ring.submit_and_wait(50));
    EXPECT_EQ(ring.ready(), 0U) << "the receive ended when its descriptor was closed";

    // It is still there, and still receives.
    ASSERT_EQ(::send(far.get(), "late", 4, MSG_NOSIGNAL), 4);
    EXPECT_EQ(reap_at_least(ring, 1), (Results{{40, 4}}));
    EXPECT_EQ(std::memcmp(buffer.data(), "late", 4), 0)
        << "the kernel wrote into the buffer after the descriptor was closed";
}

TEST_F(UringTest, ACloseThroughTheRingClosesTheDescriptor) {
    Uring ring(8);
    const int fd = near.release();
    net::prep_close(ring.get_sqe(), fd, 50);
    ASSERT_EQ(ring.submit(), 1);
    EXPECT_EQ(reap_at_least(ring, 1), (Results{{50, 0}}));
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(UringTest, AHandlerMayFillInNewEntriesWhileReaping) {
    Uring ring(8);
    net::prep_nop(ring.get_sqe(), 1);
    ASSERT_EQ(ring.submit(), 1);
    std::vector<std::uint64_t> order;
    const auto step = [&](const io_uring_cqe& cqe) {
        order.push_back(cqe.user_data);
        if (cqe.user_data < 5) {
            net::prep_nop(ring.get_sqe(), cqe.user_data + 1);
        }
    };
    for (int i = 0; i < 10 && order.size() < 5; ++i) {
        ring.reap(step);
        ASSERT_GE(ring.submit(), 0);
    }
    EXPECT_EQ(order, (std::vector<std::uint64_t>{1, 2, 3, 4, 5}));
}

TEST_F(UringTest, CountsItsSystemCalls) {
    Uring ring(8);
    for (std::uint64_t round = 1; round <= 5; ++round) {
        net::prep_nop(ring.get_sqe(), round);
        net::prep_nop(ring.get_sqe(), round + 100);
        ASSERT_EQ(ring.submit(), 2);
        ASSERT_EQ(reap(ring).size(), 2U);
        EXPECT_EQ(ring.enters(), round) << "one call a round, for two operations";
    }
}

// Not skipped anywhere: where io_uring cannot be used, making a ring throws,
// and that is all that is asked of it.
TEST(UringAvailability, IsWhatMakingARingDoes) {
#if defined(OBE_IO_URING_WORKS_HERE)
    EXPECT_TRUE(Uring::available())
        << "io_uring worked on this machine when the build was configured";
#endif
    if (Uring::available()) {
        EXPECT_NO_THROW(Uring ring(4));
    } else {
        // Either the kernel refuses outright, or it lets a ring be made and
        // not used; in the second case the constructor succeeding is right.
        try {
            const Uring ring(4);
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string_view(e.what()).find("io_uring"), std::string_view::npos);
        }
    }
}

}  // namespace
