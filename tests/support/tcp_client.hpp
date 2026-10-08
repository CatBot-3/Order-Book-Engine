#pragma once

#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "obe/gen/rng.hpp"
#include "obe/net/epoll_transport.hpp"
#include "obe/net/socket.hpp"
#include "obe/net/uring_transport.hpp"

// What the network tests share: which transport the build is testing, and a
// TCP client that a test drives by hand.
//
// tests/net/transport_test.cpp and gateway_test.cpp are built twice:
//
//   obe_net_tests                the reference transport (epoll). Part of the
//                                passing suite.
//   obe_hand_written_net_tests   built with OBE_TEST_URING defined: the
//                                hand-written io_uring transport. Labelled
//                                needs-your-code.

// What a test does when the thing it tests says it cannot run on this system.
//
// Skipping is right where that is true: an old kernel, a container that
// forbids io_uring. But "cannot run here" is the tested thing's own word, and
// a ring with a bug would say the same and have every one of its tests
// skipped. So the build asks a second opinion: a small program that shares no
// code with the library (cmake/io_uring_probe.cpp) is run when the build is
// configured, and where it found io_uring working, OBE_IO_URING_WORKS_HERE is
// defined and "cannot run here" is a failure.
//
// Use it in SetUp(), as a statement: OBE_UNAVAILABLE("the ring"); The argument
// is a string, or anything a string can be made of.
#if defined(OBE_IO_URING_WORKS_HERE)
#define OBE_UNAVAILABLE(what)                                                  \
    FAIL() << (what)                                                           \
           << " says it cannot run here, and io_uring worked on this machine " \
              "when the build was configured"
#else
#define OBE_UNAVAILABLE(what) GTEST_SKIP() << (what) << " cannot run on this system"
#endif

namespace obe::test {

#if defined(OBE_TEST_URING)
using NetUnderTest = net::UringTransport;
#else
using NetUnderTest = net::EpollTransport;
#endif

// Bytes that are a function of a seed and nothing else, so that what arrives
// can be compared with what was sent, however it was cut up on the way.
[[nodiscard]] inline std::vector<std::byte> pattern(std::uint64_t seed, std::size_t size) {
    gen::SplitMix64 rng(seed);
    std::vector<std::byte> out(size);
    for (std::byte& b : out) {
        b = static_cast<std::byte>(rng.below(256));
    }
    return out;
}

// One end of a TCP connection on loopback. Nothing in it waits: every call
// does what the socket allows at that moment and returns.
class TcpClient {
 public:
    // `receive_buffer` asks the kernel for a small receive buffer, for tests
    // about a client that does not read. It has to be set before connecting
    // to take effect.
    explicit TcpClient(std::uint16_t port, int receive_buffer = 0)
        : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        if (receive_buffer != 0) {
            ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
        }
        const sockaddr_in addr = net::make_address("127.0.0.1", port);
        if (::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            ADD_FAILURE() << net::errno_text("connect");
        }
        net::set_nodelay(fd_.get());
        net::set_nonblocking(fd_.get());
    }

    // Writes what the socket will take and returns how much that was.
    std::size_t send(std::span<const std::byte> bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t n =
                ::send(fd_.get(), bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
            if (n <= 0) {
                break;
            }
            done += static_cast<std::size_t>(n);
        }
        return done;
    }

    // Takes in whatever has arrived, adding it to received(). Returns false
    // once the other end has closed or the connection has failed.
    bool fill() {
        std::byte buf[16384];
        for (;;) {
            const ssize_t n = ::recv(fd_.get(), buf, sizeof(buf), 0);
            if (n > 0) {
                in_.insert(in_.end(), buf, buf + n);
                continue;
            }
            if (n == 0) {
                closed_ = true;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                closed_ = true;
                failed_ = true;
            }
            return !closed_;
        }
    }

    [[nodiscard]] const std::vector<std::byte>& received() const noexcept { return in_; }
    // The other end closed, or the connection failed.
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    // It ended with an error (a reset), not with an orderly close.
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    // An orderly close: what was sent is delivered, then the end.
    void close() { fd_.reset(); }

    // An abrupt one: the other end gets a reset, and what it had not read
    // may be thrown away.
    void reset() {
        const linger now{.l_onoff = 1, .l_linger = 0};
        ::setsockopt(fd_.get(), SOL_SOCKET, SO_LINGER, &now, sizeof(now));
        fd_.reset();
    }

    // No more will be sent, but the connection stays open for reading.
    void done_sending() { ::shutdown(fd_.get(), SHUT_WR); }

 private:
    net::Fd fd_;
    std::vector<std::byte> in_;
    bool closed_ = false;
    bool failed_ = false;
};

}  // namespace obe::test
