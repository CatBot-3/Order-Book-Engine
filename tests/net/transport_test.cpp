#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "obe/gen/rng.hpp"
#include "obe/net/transport.hpp"
#include "obe/net/transports.hpp"
#include "support/tcp_client.hpp"

// The contract of a transport (obe/net/transport.hpp), over real TCP sockets
// on loopback, held against whichever transport the build selects.
//
// There are no threads. The test is every client and also turns the
// transport by hand: write to a client socket, call poll() until the
// transport has reacted, read the client socket. Nothing in a transport
// happens except inside its own functions, so every test is deterministic in
// what it requires, if not in how many turns it takes to get there.
//
// Nothing here knows how a transport does its work. A test that passes on
// the readiness transport and fails on a completion one is nearly always
// about who owns a buffer while an operation is in flight; the comments say
// which tests those are.

namespace {

using namespace obe;
using namespace std::chrono_literals;
using net::ConnId;
using net::LinkState;
using test::pattern;
using test::TcpClient;

using Net = test::NetUnderTest;
using Bytes = std::vector<std::byte>;

std::span<const std::byte> piece(const Bytes& all, std::size_t at, std::size_t size) {
    return {all.data() + at, std::min(size, all.size() - at)};
}

void append(Bytes& to, std::span<const std::byte> more) {
    to.insert(to.end(), more.begin(), more.end());
}

// A handler that keeps everything it is told.
struct Recorder {
    Net* net = nullptr;

    // The ids to hand out, in order; after them, 1, 2, 3 and so on.
    std::vector<ConnId> ids_to_give;
    ConnId next_id = 1;
    std::size_t max_live = static_cast<std::size_t>(-1);

    std::vector<ConnId> connected;
    std::size_t live = 0;
    std::size_t refused = 0;
    std::map<ConnId, Bytes> received;
    std::vector<std::size_t> piece_sizes;
    std::vector<std::pair<ConnId, LinkState>> gone;
    bool echo = false;  // send back whatever arrives
    std::function<void(ConnId, std::span<const std::byte>)> also;

    ConnId on_connect() {
        if (live >= max_live) {
            ++refused;
            return 0;
        }
        ConnId id = next_id;
        if (!ids_to_give.empty()) {
            id = ids_to_give.front();
            ids_to_give.erase(ids_to_give.begin());
        } else {
            ++next_id;
        }
        connected.push_back(id);
        ++live;
        return id;
    }

    void on_data(ConnId id, std::span<const std::byte> bytes) {
        append(received[id], bytes);
        piece_sizes.push_back(bytes.size());
        if (echo) {
            net->send(id, bytes);
        }
        if (also) {
            also(id, bytes);
        }
    }

    void on_disconnect(ConnId id, LinkState why) { gone.emplace_back(id, why); }

    [[nodiscard]] std::size_t times_gone(ConnId id) const {
        return static_cast<std::size_t>(std::count_if(
            gone.begin(), gone.end(), [id](const auto& entry) { return entry.first == id; }));
    }
};

class TransportTest : public ::testing::Test {
 protected:
    void SetUp() override {
        if (!Net::available()) {
            OBE_UNAVAILABLE("the '" + std::string(Net::kName) + "' transport");
        }
    }

    void start(const net::TransportConfig& cfg = {}) {
        net = std::make_unique<Net>(cfg);
        handler.net = net.get();
        net->listen("127.0.0.1", 0);
        ASSERT_NE(net->port(), 0);
    }

    TcpClient connect(int receive_buffer = 0) { return TcpClient(net->port(), receive_buffer); }

    // One turn, as the gateway makes it.
    std::size_t turn(int timeout_ms = 1) {
        const std::size_t served = net->poll(timeout_ms, handler);
        net->flush_all(handler);
        return served;
    }

    void turns(int count) {
        for (int i = 0; i < count; ++i) {
            turn();
        }
    }

    // Turns the transport until `done` says stop. False if it took too long.
    template <class Done>
    bool turn_until(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            turn();
        }
        return true;
    }

    // A client the transport has accepted, and the id it was given.
    TcpClient accepted(ConnId& id, int receive_buffer = 0) {
        const std::size_t before = handler.connected.size();
        TcpClient client = connect(receive_buffer);
        EXPECT_TRUE(turn_until([&] { return handler.connected.size() == before + 1; }));
        id = handler.connected.empty() ? 0 : handler.connected.back();
        return client;
    }

    // With nothing to do, a poll waits out its time and serves nothing. A
    // transport that left something armed that is always ready (a socket
    // watched for writability with nothing to write, a connection whose peer
    // has gone) fails this: its loop would spin a core for nothing.
    ::testing::AssertionResult poll_is_idle() {
        turns(3);  // let anything genuinely pending finish
        const auto before = std::chrono::steady_clock::now();
        const std::size_t served = net->poll(60, handler);
        const auto waited = std::chrono::steady_clock::now() - before;
        if (served != 0) {
            return ::testing::AssertionFailure() << "an idle poll served " << served << " things";
        }
        if (waited < 50ms) {
            return ::testing::AssertionFailure()
                   << "an idle poll returned after "
                   << std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()
                   << " ms of the 60 it was given";
        }
        return ::testing::AssertionSuccess();
    }

    // Sends until the kernel refuses some of it for a client that is not
    // reading. Returns everything that was queued, in order.
    Bytes send_until_blocked(ConnId id, std::uint64_t seed) {
        Bytes all;
        for (int round = 0; round < 256 && net->stats().blocked_writes == 0; ++round) {
            const Bytes more =
                pattern(seed + static_cast<std::uint64_t>(round), std::size_t{256} * 1024);
            net->send(id, more);
            append(all, more);
            EXPECT_EQ(net->flush(id), LinkState::Up);
        }
        EXPECT_GT(net->stats().blocked_writes, 0U) << "64 MiB went to a client that reads nothing";
        return all;
    }

    Recorder handler;
    std::unique_ptr<Net> net;
};

// --- Listening and accepting ---------------------------------------------------------

TEST_F(TransportTest, ListensOnAPortTheSystemChoosesAndSaysWhich) {
    start();
    EXPECT_NE(net->port(), 0);
    // Another transport gets another port.
    Net other{net::TransportConfig{}};
    other.listen("127.0.0.1", 0);
    EXPECT_NE(other.port(), 0);
    EXPECT_NE(other.port(), net->port());
}

TEST_F(TransportTest, HasANameAndCanBeFoundByIt) {
    EXPECT_FALSE(Net::kName.empty());
    EXPECT_FALSE(Net::kDescription.empty());
    bool found = false;
    EXPECT_TRUE(net::with_transport(
        Net::kName, [&found]<class T>(std::type_identity<T>) { found = std::is_same_v<T, Net>; }));
    EXPECT_TRUE(found);
    EXPECT_FALSE(net::with_transport("carrier-pigeon", [](auto) { ADD_FAILURE(); }));
    int count = 0;
    net::for_each_transport([&count](auto) { ++count; });
    EXPECT_EQ(count, 2);
}

TEST_F(TransportTest, EachConnectionIsKnownByTheIdTheHandlerGaveIt) {
    start();
    handler.ids_to_give = {700, 3, 41};
    ConnId a = 0;
    ConnId b = 0;
    ConnId c = 0;
    TcpClient first = accepted(a);
    TcpClient second = accepted(b);
    TcpClient third = accepted(c);
    EXPECT_EQ(a, 700U);
    EXPECT_EQ(b, 3U);
    EXPECT_EQ(c, 41U);

    const Bytes one = pattern(1, 100);
    const Bytes two = pattern(2, 200);
    const Bytes three = pattern(3, 300);
    // In another order than they connected in.
    ASSERT_EQ(third.send(three), three.size());
    ASSERT_EQ(first.send(one), one.size());
    ASSERT_EQ(second.send(two), two.size());
    ASSERT_TRUE(turn_until([&] {
        return handler.received[700].size() == 100 && handler.received[3].size() == 200 &&
               handler.received[41].size() == 300;
    }));
    EXPECT_EQ(handler.received[700], one);
    EXPECT_EQ(handler.received[3], two);
    EXPECT_EQ(handler.received[41], three);

    // And what is sent to an id goes to that connection and no other.
    net->send(3, two);
    net->send(700, one);
    ASSERT_TRUE(turn_until([&] {
        first.fill();
        second.fill();
        return first.received().size() == 100 && second.received().size() == 200;
    }));
    EXPECT_EQ(first.received(), one);
    EXPECT_EQ(second.received(), two);
    third.fill();
    EXPECT_TRUE(third.received().empty());
    EXPECT_TRUE(handler.gone.empty());
}

TEST_F(TransportTest, AConnectionTheHandlerRefusesIsClosedAndNeverHeardOf) {
    start();
    handler.max_live = 1;
    ConnId kept = 0;
    TcpClient welcome = accepted(kept);
    TcpClient turned_away = connect();
    ASSERT_TRUE(turn_until([&] { return handler.refused == 1; }));
    static_cast<void>(turned_away.send(pattern(4, 50)));
    ASSERT_TRUE(turn_until([&] {
        turned_away.fill();
        return turned_away.closed();
    }));
    turns(5);
    EXPECT_EQ(handler.connected.size(), 1U);
    EXPECT_EQ(handler.received.count(0), 0U) << "data arrived under the id that means 'refused'";
    EXPECT_TRUE(handler.gone.empty()) << "a connection that was never accepted was reported";

    // The one that was accepted is unharmed.
    const Bytes hello = pattern(5, 64);
    ASSERT_EQ(welcome.send(hello), hello.size());
    ASSERT_TRUE(turn_until([&] { return handler.received[kept].size() == hello.size(); }));
    EXPECT_EQ(handler.received[kept], hello);
}

TEST_F(TransportTest, ManyConnectionsEachKeepTheirOwnBytes) {
    start();
    handler.echo = true;
    constexpr std::size_t kClients = 200;
    std::vector<TcpClient> clients;
    clients.reserve(kClients);
    for (std::size_t i = 0; i < kClients; ++i) {
        clients.push_back(connect());
        if (i % 16 == 15) {
            turn();
        }
    }
    ASSERT_TRUE(turn_until([&] { return handler.connected.size() == kClients; }));
    // Each client says something only it would say. Which id a client got
    // depends on the order the kernel handed the connections over, so the
    // check is on what comes back to the client.
    std::vector<Bytes> said(kClients);
    for (std::size_t i = 0; i < kClients; ++i) {
        said[i] = pattern(1'000 + i, 40 + i);
        ASSERT_EQ(clients[i].send(said[i]), said[i].size());
    }
    ASSERT_TRUE(turn_until([&] {
        bool all = true;
        for (std::size_t i = 0; i < kClients; ++i) {
            clients[i].fill();
            all = all && clients[i].received().size() >= said[i].size();
        }
        return all;
    }));
    for (std::size_t i = 0; i < kClients; ++i) {
        ASSERT_EQ(clients[i].received(), said[i]) << "client " << i;
    }
    EXPECT_TRUE(handler.gone.empty());
}

// --- Receiving -----------------------------------------------------------------------

TEST_F(TransportTest, BytesArriveInOrderHoweverTheyWereCutUp) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id);
    const Bytes all = pattern(11, 300'000);
    gen::SplitMix64 rng(12);
    std::size_t sent = 0;
    ASSERT_TRUE(turn_until([&] {
        if (sent < all.size()) {
            sent += client.send(piece(all, sent, 1 + rng.below(9'000)));
        }
        return handler.received[id].size() == all.size();
    }));
    EXPECT_EQ(handler.received[id], all);
    EXPECT_EQ(net->stats().bytes_in, all.size());
}

TEST_F(TransportTest, NoPieceIsLargerThanTheReadChunk) {
    start({.read_chunk = 48});
    ConnId id = 0;
    TcpClient client = accepted(id);
    const Bytes all = pattern(13, 5'000);
    ASSERT_EQ(client.send(all), all.size());
    ASSERT_TRUE(turn_until([&] { return handler.received[id].size() == all.size(); }));
    EXPECT_EQ(handler.received[id], all);
    for (const std::size_t size : handler.piece_sizes) {
        ASSERT_GE(size, 1U) << "an empty piece is not data";
        ASSERT_LE(size, 48U);
    }
}

TEST_F(TransportTest, WhatAClientSentBeforeClosingArrivesBeforeTheClosing) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id);
    const Bytes all = pattern(14, 150'000);
    std::size_t sent = 0;
    ASSERT_TRUE(turn_until([&] {
        sent += client.send(piece(all, sent, all.size()));
        return sent == all.size();
    }));
    client.close();
    ASSERT_TRUE(turn_until([&] { return handler.times_gone(id) != 0; }));
    EXPECT_EQ(handler.received[id], all) << "the end was reported before the data";
    EXPECT_EQ(handler.gone.front(), std::make_pair(id, LinkState::ClosedByPeer));
}

// --- Sending -------------------------------------------------------------------------

TEST_F(TransportTest, WhatIsSentArrivesInOrder) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id);
    const Bytes all = pattern(21, 200'000);
    gen::SplitMix64 rng(22);
    std::size_t queued = 0;
    ASSERT_TRUE(turn_until([&] {
        // A few pieces a turn, of every size from one byte up.
        for (int i = 0; i < 5 && queued < all.size(); ++i) {
            const std::span<const std::byte> next = piece(all, queued, 1 + rng.below(3'000));
            net->send(id, next);
            queued += next.size();
        }
        client.fill();
        return client.received().size() == all.size();
    }));
    EXPECT_EQ(client.received(), all);
    // The client has it all. A transport that learns of a finished send
    // through a completion may need a turn more to know that itself.
    ASSERT_TRUE(turn_until([&] { return net->unsent(id) == 0; }));
    EXPECT_EQ(net->stats().bytes_out, all.size());
}

// send() copies. The caller's buffer is its own again the moment send
// returns, whatever the transport is still doing with the bytes.
TEST_F(TransportTest, TheCallersBufferIsItsOwnAgainWhenSendReturns) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id);
    const Bytes all = pattern(23, 50'000);
    {
        Bytes scratch = all;
        net->send(id, scratch);
        std::fill(scratch.begin(), scratch.end(), std::byte{0xEE});
    }  // and freed
    ASSERT_TRUE(turn_until([&] {
        client.fill();
        return client.received().size() == all.size();
    }));
    EXPECT_EQ(client.received(), all);
}

// What the gateway's judgement of a slow client rests on: after flush(id),
// unsent(id) is the truth, at once, without another turn.
TEST_F(TransportTest, FlushHandsOverAtOnceWhatTheKernelWillTake) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id);
    const Bytes all = pattern(24, 4'000);
    net->send(id, all);
    EXPECT_EQ(net->unsent(id), all.size());
    EXPECT_EQ(net->flush(id), LinkState::Up);
    EXPECT_EQ(net->unsent(id), 0U) << "an idle socket has room for 4000 bytes";
    // No poll in between: the bytes are already the kernel's.
    for (int i = 0; i < 200 && client.received().size() < all.size(); ++i) {
        client.fill();
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(client.received(), all);

    // Again and again, as a burst of answers in one turn would.
    Bytes more;
    for (int i = 0; i < 300; ++i) {
        const Bytes next = pattern(100 + static_cast<std::uint64_t>(i), 700);
        net->send(id, next);
        append(more, next);
        ASSERT_EQ(net->flush(id), LinkState::Up);
        client.fill();  // it is reading
        ASSERT_LE(net->unsent(id), 700U * 4) << "a backlog built up behind a client that reads";
    }
    ASSERT_TRUE(turn_until([&] {
        client.fill();
        return client.received().size() == all.size() + more.size();
    }));
    Bytes expected = all;
    append(expected, more);
    EXPECT_EQ(client.received(), expected);
}

// unsent(id) is the bytes the kernel has not taken, no more and no less: not
// what the transport happens to be holding in memory, which may include a
// part already sent. One large piece for a client that reads nothing, so
// that the kernel takes a beginning of it and stops.
TEST_F(TransportTest, UnsentIsWhatTheKernelHasNotTakenAndNothingElse) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id, 4096);
    const Bytes all = pattern(25, std::size_t{16} * 1024 * 1024);
    net->send(id, all);
    EXPECT_EQ(net->unsent(id), all.size());
    ASSERT_EQ(net->flush(id), LinkState::Up);
    const std::uint64_t taken = net->stats().bytes_out;
    ASSERT_GT(taken, 0U) << "the kernel took nothing at all";
    ASSERT_LT(taken, all.size() / 2) << "the kernel took most of 16 MiB for a client that reads "
                                        "nothing: this test needs a larger piece";
    EXPECT_EQ(net->unsent(id), all.size() - taken);
    EXPECT_GT(net->stats().blocked_writes, 0U);
    // And the same after a few turns in which nothing can move.
    turns(3);
    EXPECT_EQ(net->unsent(id) + net->stats().bytes_out, all.size());
}

TEST_F(TransportTest, AClientThatDoesNotReadLeavesABacklogThatDrainsWhenItDoes) {
    start();
    ConnId slow_id = 0;
    ConnId good_id = 0;
    TcpClient slow = accepted(slow_id, 4096);
    TcpClient good = accepted(good_id);
    const Bytes all = send_until_blocked(slow_id, 31);
    const std::size_t stuck = net->unsent(slow_id);
    EXPECT_GT(stuck, 0U);
    EXPECT_LT(stuck, all.size()) << "the kernel took none of it";

    // Turning does not make the backlog go away while the client does not
    // read, and the other client is served all the while. (The backlog may
    // shrink a little: the kernel is free to find room for a few more bytes
    // when an acknowledgement comes in late. It cannot grow.)
    const Bytes hello = pattern(32, 500);
    net->send(good_id, hello);
    turns(10);
    EXPECT_LE(net->unsent(slow_id), stuck);
    EXPECT_GT(net->unsent(slow_id), 0U);
    good.fill();
    EXPECT_EQ(good.received(), hello);
    EXPECT_TRUE(handler.gone.empty()) << "a slow client is not a dead one";

    // Far more is queued behind it, and then it starts reading. Nothing but
    // the socket having room prompts the transport to carry on. All the way
    // down, what is unsent and what was sent add up to what was queued: the
    // count is of bytes the kernel has not taken, and a buffer that is half
    // sent is half unsent.
    Bytes all_of_it = all;
    const Bytes more = pattern(33, std::size_t{3} * 1024 * 1024);
    net->send(slow_id, more);
    append(all_of_it, more);
    std::size_t turns_taken = 0;
    std::size_t turns_that_did_not_add_up = 0;
    ASSERT_TRUE(turn_until([&] {
        slow.fill();
        ++turns_taken;
        if (net->unsent(slow_id) + net->stats().bytes_out - hello.size() != all_of_it.size()) {
            ++turns_that_did_not_add_up;
        }
        return slow.received().size() == all_of_it.size();
    }));
    EXPECT_EQ(turns_that_did_not_add_up, 0U) << "of " << turns_taken << " turns";
    EXPECT_TRUE(slow.received() == all_of_it);
    ASSERT_TRUE(turn_until([&] { return net->unsent(slow_id) == 0; }));
    EXPECT_FALSE(slow.closed());
    // With the backlog gone, the transport is no longer waiting for room.
    EXPECT_TRUE(poll_is_idle());
}

// The test about buffers. While part of a connection's output is with the
// kernel (or waiting to be), more is queued behind it, a little at a time,
// enough for any growing buffer to have moved many times over. What arrives
// must be every byte, once, in order.
TEST_F(TransportTest, QueueingBehindASendThatIsUnderWayKeepsEveryByteInPlace) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id, 4096);
    Bytes all = send_until_blocked(id, 41);
    ASSERT_GT(net->unsent(id), 0U);

    gen::SplitMix64 rng(42);
    for (int i = 0; i < 400; ++i) {
        const Bytes more = pattern(5'000 + static_cast<std::uint64_t>(i), 1 + rng.below(20'000));
        net->send(id, more);
        append(all, more);
        if (i % 3 == 0) {
            turn(0);
        }
        if (i % 7 == 0) {
            EXPECT_EQ(net->flush(id), LinkState::Up);
        }
        if (i % 50 == 49) {
            client.fill();  // lets some of it move, so that sends complete in between
        }
    }
    EXPECT_EQ(net->unsent(id) + net->stats().bytes_out, all.size())
        << "bytes queued, bytes sent and bytes waiting do not add up";
    ASSERT_TRUE(turn_until([&] {
        client.fill();
        return client.received().size() >= all.size() || client.closed();
    }));
    ASSERT_EQ(client.received().size(), all.size());
    EXPECT_TRUE(client.received() == all) << "the bytes arrived damaged or out of order";
}

TEST_F(TransportTest, AHandlerMaySendAndFlushFromInsideACallback) {
    start();
    ConnId a = 0;
    ConnId b = 0;
    TcpClient ann = accepted(a);
    TcpClient bob = accepted(b);
    // Whatever Ann says is flushed straight back to her, and passed on to
    // Bob, from inside on_data.
    handler.also = [&](ConnId id, std::span<const std::byte> bytes) {
        if (id == a) {
            net->send(a, bytes);
            EXPECT_EQ(net->flush(a), LinkState::Up);
            net->send(b, bytes);
            EXPECT_EQ(net->unsent(b) >= bytes.size(), true);
        }
    };
    const Bytes all = pattern(51, 120'000);
    std::size_t sent = 0;
    ASSERT_TRUE(turn_until([&] {
        sent += ann.send(piece(all, sent, 4'000));
        ann.fill();
        bob.fill();
        return ann.received().size() == all.size() && bob.received().size() == all.size();
    }));
    EXPECT_EQ(ann.received(), all);
    EXPECT_EQ(bob.received(), all);
    EXPECT_EQ(handler.received[a], all);
}

TEST_F(TransportTest, AnEchoOfEverythingFromManyClientsAtOnce) {
    start({.read_chunk = 1024});
    handler.echo = true;
    constexpr std::size_t kClients = 24;
    std::vector<TcpClient> clients;
    std::vector<Bytes> said;
    std::vector<std::size_t> sent(kClients, 0);
    for (std::size_t i = 0; i < kClients; ++i) {
        clients.push_back(connect());
        said.push_back(pattern(600 + i, 60'000 + 1'000 * i));
    }
    gen::SplitMix64 rng(61);
    ASSERT_TRUE(turn_until([&] {
        bool all = true;
        for (std::size_t i = 0; i < kClients; ++i) {
            if (sent[i] < said[i].size() && rng.below(3) != 0) {
                sent[i] += clients[i].send(piece(said[i], sent[i], 1 + rng.below(5'000)));
            }
            clients[i].fill();
            all = all && clients[i].received().size() == said[i].size();
        }
        return all;
    }));
    for (std::size_t i = 0; i < kClients; ++i) {
        ASSERT_TRUE(clients[i].received() == said[i]) << "client " << i;
    }
    ASSERT_TRUE(turn_until([&] { return net->stats().bytes_out == net->stats().bytes_in; }));
}

// --- Connections that end ------------------------------------------------------------

TEST_F(TransportTest, AClientThatClosesIsReportedOnce) {
    start();
    ConnId id = 0;
    ConnId other_id = 0;
    TcpClient client = accepted(id);
    TcpClient other = accepted(other_id);
    client.close();
    ASSERT_TRUE(turn_until([&] { return !handler.gone.empty(); }));
    EXPECT_EQ(handler.gone.front(), std::make_pair(id, LinkState::ClosedByPeer));
    // It stays reported once, however long the handler takes to close it,
    // and sending to it meanwhile does no harm.
    net->send(id, pattern(71, 100));
    turns(20);
    EXPECT_EQ(handler.times_gone(id), 1U);
    EXPECT_EQ(handler.gone.size(), 1U);
    EXPECT_EQ(net->unsent(id), 0U) << "bytes for a dead connection are dropped, not kept";
    // A dead connection the handler has not closed yet costs nothing.
    EXPECT_TRUE(poll_is_idle());
    net->close(id);
    turns(5);
    EXPECT_EQ(handler.gone.size(), 1U);

    // The other connection never noticed.
    const Bytes hello = pattern(72, 300);
    ASSERT_EQ(other.send(hello), hello.size());
    ASSERT_TRUE(turn_until([&] { return handler.received[other_id].size() == hello.size(); }));
}

TEST_F(TransportTest, AClientThatResetsIsClosedByPeerToo) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id);
    client.reset();
    ASSERT_TRUE(turn_until([&] { return !handler.gone.empty(); }));
    EXPECT_EQ(handler.gone.front(), std::make_pair(id, LinkState::ClosedByPeer));
    turns(10);
    EXPECT_EQ(handler.gone.size(), 1U);
}

TEST_F(TransportTest, WritingToAClientThatHasGoneIsFoundOutAndReportedOnce) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id);
    client.reset();
    // Only sending and turning: sooner or later the transport notices,
    // whether by reading or by writing.
    ASSERT_TRUE(turn_until([&] {
        net->send(id, pattern(73, 2'000));
        return handler.times_gone(id) != 0;
    }));
    for (int i = 0; i < 20; ++i) {
        net->send(id, pattern(74, 2'000));
        turn();
    }
    EXPECT_EQ(handler.times_gone(id), 1U);
    EXPECT_EQ(handler.gone.front().second, LinkState::ClosedByPeer)
        << "a reset is the client's doing, whether a read or a write ran into it";
}

// flush(id) is the other way a death is reported. Whoever asked has been
// told, and is not told again through the handler.
TEST_F(TransportTest, ADeathThatFlushReportsIsNotReportedAgain) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id);
    client.reset();
    LinkState state = LinkState::Up;
    // No poll: only flush(id) can find out.
    for (int i = 0; i < 2'000 && state == LinkState::Up; ++i) {
        net->send(id, pattern(75, 1'000));
        state = net->flush(id);
        if (state == LinkState::Up) {
            std::this_thread::sleep_for(1ms);
        }
    }
    ASSERT_NE(state, LinkState::Up) << "two thousand writes to a reset connection all succeeded";
    EXPECT_EQ(state, LinkState::ClosedByPeer);
    turns(20);
    EXPECT_EQ(handler.times_gone(id), 0U) << "reported a second time";
    EXPECT_EQ(net->flush(id), state) << "and it stays dead";
    // Found dead this way or the other, it costs nothing while it waits to
    // be closed.
    EXPECT_TRUE(poll_is_idle());
    net->close(id);
    turns(5);
    EXPECT_TRUE(handler.gone.empty());
}

TEST_F(TransportTest, ClosingAConnectionEndsItForTheClientAndSaysNothingMore) {
    start();
    ConnId id = 0;
    ConnId other_id = 0;
    TcpClient client = accepted(id);
    TcpClient other = accepted(other_id);
    const Bytes last = pattern(81, 900);
    net->send(id, last);
    turn();
    net->close(id);
    ASSERT_TRUE(turn_until([&] {
        client.fill();
        return client.closed();
    }));
    EXPECT_EQ(client.received(), last) << "what was already on its way still arrived";
    EXPECT_EQ(net->unsent(id), 0U);
    // Whatever the client does now is nobody's business.
    static_cast<void>(client.send(pattern(82, 5'000)));
    client.close();
    turns(20);
    EXPECT_TRUE(handler.gone.empty()) << "something was reported about a closed connection";
    EXPECT_EQ(handler.received.count(id), 0U);

    const Bytes hello = pattern(83, 300);
    ASSERT_EQ(other.send(hello), hello.size());
    ASSERT_TRUE(turn_until([&] { return handler.received[other_id].size() == hello.size(); }));
}

// send() only queues. With no backlog on the connection, nothing is offered
// to the kernel before a flush, so what is queued for a connection that is
// closed first never leaves. The gateway relies on it: a client it drops in
// the middle of a turn is sent nothing of what that turn had queued for it.
TEST_F(TransportTest, WhatIsQueuedForAConnectionClosedBeforeAFlushIsNeverSent) {
    start();
    ConnId id = 0;
    ConnId other_id = 0;
    TcpClient client = accepted(id);
    TcpClient other = accepted(other_id);
    turns(3);
    const std::uint64_t out_before = net->stats().bytes_out;

    net->send(id, pattern(84, 700));
    EXPECT_EQ(net->unsent(id), 700U);
    // A poll is not a flush either.
    EXPECT_EQ(net->poll(0, handler), 0U);
    EXPECT_EQ(net->unsent(id), 700U);
    net->close(id);
    EXPECT_EQ(net->unsent(id), 0U);

    ASSERT_TRUE(turn_until([&] {
        client.fill();
        return client.closed();
    }));
    EXPECT_TRUE(client.received().empty()) << "bytes left for a connection closed before a flush";
    EXPECT_EQ(net->stats().bytes_out, out_before);

    // The other connection's bytes, queued in the same turn, do go.
    const Bytes hello = pattern(85, 300);
    net->send(other_id, hello);
    ASSERT_TRUE(turn_until([&] {
        other.fill();
        return other.received().size() == hello.size();
    }));
    EXPECT_EQ(other.received(), hello);
    EXPECT_TRUE(handler.gone.empty());
}

// A connection is closed while the transport is still waiting to hear from
// it, and its id is given to the next client at once. Nothing that was meant
// for the old connection, or came from it, may turn up on the new one.
TEST_F(TransportTest, AnIdCanBeUsedAgainAsSoonAsItsConnectionIsClosed) {
    start();
    for (int round = 0; round < 30; ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);
        handler.ids_to_give = {5};
        ConnId id = 0;
        TcpClient old_client = accepted(id);
        ASSERT_EQ(id, 5U);
        if (round % 3 == 1) {
            turns(2);  // let the transport settle into waiting for it
        }
        if (round % 3 == 2) {
            // The hardest order of events for a completion transport: the old
            // client's bytes have ARRIVED, and the transport has not been
            // given a turn to hear of it. Whatever it had waiting on the
            // connection is finished, with data, and uncollected, at the
            // moment the connection is closed and its id handed on.
            static_cast<void>(
                old_client.send(pattern(800 + static_cast<std::uint64_t>(round), 3'000)));
        }
        net->close(5);
        // The old client talks on, to nobody.
        static_cast<void>(old_client.send(pattern(900 + static_cast<std::uint64_t>(round), 3'000)));

        handler.ids_to_give = {5};
        TcpClient new_client = accepted(id);
        ASSERT_EQ(id, 5U);
        const Bytes said = pattern(950 + static_cast<std::uint64_t>(round), 2'000);
        ASSERT_EQ(new_client.send(said), said.size());
        const Bytes answer = pattern(990 + static_cast<std::uint64_t>(round), 1'500);
        net->send(5, answer);
        ASSERT_TRUE(turn_until([&] {
            new_client.fill();
            return handler.received[5].size() >= said.size() &&
                   new_client.received().size() >= answer.size();
        }));
        ASSERT_EQ(handler.received[5], said) << "bytes from the old connection under the new id";
        ASSERT_EQ(new_client.received(), answer);
        old_client.fill();
        ASSERT_TRUE(old_client.received().empty()) << "the old client got the new one's answer";
        ASSERT_TRUE(handler.gone.empty()) << "the old connection's end was reported under the id";

        net->close(5);
        handler.received.erase(5);
        turns(2);
    }
}

// Closing with output still queued, and more: with a send the kernel has not
// finished. The bytes are discarded; what must survive is everybody else.
TEST_F(TransportTest, ClosingAConnectionWithABacklogDiscardsItAndHarmsNobody) {
    start();
    handler.echo = true;
    ConnId good_id = 0;
    TcpClient good = accepted(good_id);
    Bytes good_said;
    for (int round = 0; round < 8; ++round) {
        SCOPED_TRACE(::testing::Message() << "round " << round);
        const std::uint64_t blocked_before = net->stats().blocked_writes;
        ConnId id = 0;
        TcpClient slow = accepted(id, 4096);
        Bytes queued;
        for (int i = 0; i < 256 && net->stats().blocked_writes == blocked_before; ++i) {
            const Bytes more =
                pattern(2'000 + static_cast<std::uint64_t>(i), std::size_t{256} * 1024);
            net->send(id, more);
            append(queued, more);
            ASSERT_EQ(net->flush(id), LinkState::Up);
        }
        ASSERT_GT(net->unsent(id), 0U);
        net->close(id);
        EXPECT_EQ(net->unsent(id), 0U);

        // The transport goes on serving while the closed connection's
        // operations, if it had any in flight, come to their end.
        const Bytes more = pattern(3'000 + static_cast<std::uint64_t>(round), 10'000);
        ASSERT_EQ(good.send(more), more.size());
        append(good_said, more);
        ASSERT_TRUE(turn_until([&] {
            good.fill();
            slow.fill();
            return good.received().size() == good_said.size();
        }));
        // The slow client got a beginning of what was queued and then the end.
        ASSERT_TRUE(turn_until([&] {
            slow.fill();
            return slow.closed();
        }));
        ASSERT_LE(slow.received().size(), queued.size());
        ASSERT_TRUE(std::equal(slow.received().begin(), slow.received().end(), queued.begin()));
    }
    EXPECT_EQ(good.received(), good_said);
    EXPECT_TRUE(handler.gone.empty());
}

TEST_F(TransportTest, IdsNobodyHasMeanNothing) {
    start();
    net->send(999, pattern(91, 100));
    EXPECT_EQ(net->unsent(999), 0U);
    EXPECT_NE(net->flush(999), LinkState::Up);
    net->close(999);
    net->close(0);
    turns(3);
    EXPECT_TRUE(handler.gone.empty());
    EXPECT_TRUE(handler.received.empty());
    // And everything still works.
    ConnId id = 0;
    TcpClient client = accepted(id);
    const Bytes hello = pattern(92, 10);
    ASSERT_EQ(client.send(hello), hello.size());
    ASSERT_TRUE(turn_until([&] { return handler.received[id] == hello; }));
}

// The last test of the hard rule: a transport that goes away with
// connections open, output queued and operations in flight must leave
// nothing behind, and nothing of its own for the kernel to write into.
TEST_F(TransportTest, ATransportThatIsDestroyedClosesEverything) {
    start();
    ConnId idle_id = 0;
    ConnId slow_id = 0;
    TcpClient idle = accepted(idle_id);
    TcpClient slow = accepted(slow_id, 4096);
    static_cast<void>(send_until_blocked(slow_id, 95));
    TcpClient not_yet_accepted = connect();

    net.reset();

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!(idle.closed() && slow.closed() && not_yet_accepted.closed()) &&
           std::chrono::steady_clock::now() < deadline) {
        idle.fill();
        slow.fill();
        not_yet_accepted.fill();
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(idle.closed());
    EXPECT_TRUE(slow.closed());
    EXPECT_TRUE(not_yet_accepted.closed());
}

// --- Waiting and counting ------------------------------------------------------------

TEST_F(TransportTest, PollWaitsAsLongAsItIsToldToAndNoLonger) {
    start();
    ConnId id = 0;
    TcpClient client = accepted(id);
    turns(3);  // whatever accepting left to do is done

    auto before = std::chrono::steady_clock::now();
    EXPECT_EQ(net->poll(0, handler), 0U);
    EXPECT_LT(std::chrono::steady_clock::now() - before, 500ms) << "a poll of 0 waited";

    before = std::chrono::steady_clock::now();
    EXPECT_EQ(net->poll(60, handler), 0U) << "nothing happened, yet something was served";
    const auto waited = std::chrono::steady_clock::now() - before;
    EXPECT_GE(waited, 50ms) << "an idle poll returned early: a loop around it would spin";
    EXPECT_LT(waited, 5s);

    // With something to do it does not wait out its time.
    ASSERT_EQ(client.send(pattern(96, 20)), 20U);
    before = std::chrono::steady_clock::now();
    EXPECT_GE(net->poll(5'000, handler), 1U);
    EXPECT_LT(std::chrono::steady_clock::now() - before, 4s);
    EXPECT_EQ(handler.received[id].size(), 20U);
}

TEST_F(TransportTest, CountsBytesAndSystemCalls) {
    start();
    EXPECT_EQ(net->stats().bytes_in, 0U);
    EXPECT_EQ(net->stats().bytes_out, 0U);
    EXPECT_EQ(net->stats().blocked_writes, 0U);
    ConnId id = 0;
    TcpClient client = accepted(id);
    const std::uint64_t after_accept = net->stats().syscalls;
    EXPECT_GT(after_accept, 0U);

    const Bytes in = pattern(97, 1'234);
    const Bytes out = pattern(98, 4'321);
    ASSERT_EQ(client.send(in), in.size());
    ASSERT_TRUE(turn_until([&] { return handler.received[id].size() == in.size(); }));
    net->send(id, out);
    ASSERT_TRUE(turn_until([&] {
        client.fill();
        return client.received().size() == out.size();
    }));
    ASSERT_TRUE(turn_until([&] { return net->unsent(id) == 0; }));
    const net::TransportStats stats = net->stats();
    EXPECT_EQ(stats.bytes_in, 1'234U);
    EXPECT_EQ(stats.bytes_out, 4'321U);
    EXPECT_EQ(stats.blocked_writes, 0U);
    EXPECT_GT(stats.syscalls, after_accept);
    // Not a performance claim, only a check that the count is of the right
    // kind: this exchange is a handful of turns, not thousands of calls.
    EXPECT_LT(stats.syscalls - after_accept, 2'000U);
}

}  // namespace
