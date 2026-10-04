#include <gtest/gtest.h>

#include <sys/epoll.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/engine/reference_engine.hpp"
#include "obe/engine/round_trip.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/net/epoll_loop.hpp"
#include "obe/net/moldudp.hpp"
#include "obe/net/socket.hpp"
#include "obe/net/udp.hpp"
#include "support/trace_handler.hpp"

// The sockets under the market-data feed, and the small helpers in
// obe/net/socket.hpp. Everything here stays on the loopback interface.

namespace {

using namespace obe;
using namespace std::chrono_literals;

// Reads datagrams from `socket` into `on_packet` until `done` says stop or
// two seconds pass without it.
template <class OnPacket, class Done>
bool receive_until(net::UdpReceiver& socket, OnPacket on_packet, Done done) {
    net::EpollLoop loop;
    loop.add(socket.fd(), EPOLLIN, 1);
    std::array<std::byte, 2048> buf{};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        static_cast<void>(loop.wait(20));
        for (;;) {
            const std::ptrdiff_t n = socket.receive(buf);
            if (n < 0) {
                break;
            }
            on_packet(std::span<const std::byte>(buf.data(), static_cast<std::size_t>(n)));
        }
    }
    return true;
}

struct SendTo {
    net::UdpSender* socket;
    void operator()(std::span<const std::byte> packet) const {
        static_cast<void>(socket->send(packet));
    }
};

TEST(Endpoint, ParsesHostAndPort) {
    net::Endpoint e;
    ASSERT_TRUE(net::parse_endpoint("127.0.0.1:9001", e));
    EXPECT_EQ(e.host, "127.0.0.1");
    EXPECT_EQ(e.port, 9001);
    ASSERT_TRUE(net::parse_endpoint("239.255.42.1:0", e));
    EXPECT_EQ(e.host, "239.255.42.1");
    EXPECT_EQ(e.port, 0);
}

TEST(Endpoint, RefusesAnythingElseAndLeavesTheOutputAlone) {
    net::Endpoint e{"1.2.3.4", 5};
    for (const char* bad :
         {"", "127.0.0.1", ":9001", "127.0.0.1:", "127.0.0.1:port", "127.0.0.1:70000",
          "127.0.0.1:-1", "localhost:9001", "1.2.3:80", "1.2.3.4.5:80", "127.0.0.1:9001x"}) {
        EXPECT_FALSE(net::parse_endpoint(bad, e)) << "'" << bad << "'";
    }
    EXPECT_EQ(e.host, "1.2.3.4");
    EXPECT_EQ(e.port, 5);
}

TEST(Endpoint, KnowsAMulticastAddress) {
    EXPECT_TRUE(net::is_multicast("224.0.0.1"));
    EXPECT_TRUE(net::is_multicast("239.255.255.255"));
    EXPECT_FALSE(net::is_multicast("223.255.255.255"));
    EXPECT_FALSE(net::is_multicast("240.0.0.1"));
    EXPECT_FALSE(net::is_multicast("127.0.0.1"));
    EXPECT_FALSE(net::is_multicast("not an address"));
}

TEST(Fd, ClosesWhatItOwnsAndMovesOwnership) {
    net::Fd none;
    EXPECT_FALSE(none.valid());
    net::Fd a = net::listen_tcp("127.0.0.1", 0);
    ASSERT_TRUE(a.valid());
    const int raw = a.get();
    net::Fd b = std::move(a);
    // The moved-from state of an Fd is defined: it owns nothing.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_FALSE(a.valid());
    EXPECT_EQ(b.get(), raw);
    b.reset();
    EXPECT_FALSE(b.valid());
}

TEST(Tcp, ListenOnPortZeroPicksAPortAndConnectReachesIt) {
    const net::Fd listener = net::listen_tcp("127.0.0.1", 0);
    const std::uint16_t port = net::local_port(listener.get());
    EXPECT_NE(port, 0);
    const net::Fd client = net::connect_tcp("127.0.0.1", port, true);
    EXPECT_TRUE(client.valid());
}

TEST(Tcp, SetUpFailuresThrowWithAReason) {
    EXPECT_THROW(static_cast<void>(net::listen_tcp("not an address", 0)), std::runtime_error);
    const net::Fd taken = net::listen_tcp("127.0.0.1", 0);
    const std::uint16_t port = net::local_port(taken.get());
    // Nothing is listening on a port that was bound and released.
    std::uint16_t free_port = 0;
    {
        const net::Fd probe = net::listen_tcp("127.0.0.1", 0);
        free_port = net::local_port(probe.get());
    }
    EXPECT_THROW(static_cast<void>(net::connect_tcp("127.0.0.1", free_port, false)),
                 std::runtime_error);
    EXPECT_NE(port, free_port);
}

TEST(Udp, ADatagramSentIsADatagramReceived) {
    net::UdpReceiver receiver("127.0.0.1", 0);
    ASSERT_NE(receiver.port(), 0);
    net::UdpSender sender("127.0.0.1", receiver.port());

    std::array<std::byte, 64> none{};
    EXPECT_EQ(receiver.receive(none), -1) << "nothing sent yet, and it must not wait";

    const std::vector<std::byte> hello{std::byte{'h'}, std::byte{'i'}, std::byte{0}, std::byte{9}};
    ASSERT_TRUE(sender.send(hello));
    std::vector<std::vector<std::byte>> got;
    ASSERT_TRUE(receive_until(
        receiver, [&](std::span<const std::byte> p) { got.emplace_back(p.begin(), p.end()); },
        [&] { return !got.empty(); }));
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0], hello);
    EXPECT_EQ(sender.sent(), 1U);
    EXPECT_EQ(sender.dropped(), 0U);
}

TEST(Udp, DatagramsKeepTheirBoundaries) {
    net::UdpReceiver receiver("127.0.0.1", 0);
    net::UdpSender sender("127.0.0.1", receiver.port());
    for (std::size_t size = 1; size <= 40; ++size) {
        const std::vector<std::byte> packet(size, static_cast<std::byte>(size));
        ASSERT_TRUE(sender.send(packet));
    }
    std::vector<std::size_t> sizes;
    ASSERT_TRUE(receive_until(
        receiver, [&](std::span<const std::byte> p) { sizes.push_back(p.size()); },
        [&] { return sizes.size() == 40; }));
    // Unlike TCP, each send arrives as one unit of exactly its own size. On
    // loopback nothing reorders them either.
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        EXPECT_EQ(sizes[i], i + 1);
    }
}

TEST(Udp, ADatagramTheKernelRefusesIsCountedAsDropped) {
    net::UdpReceiver receiver("127.0.0.1", 0);
    net::UdpSender sender("127.0.0.1", receiver.port());
    // Larger than any UDP datagram can be (65,507 bytes of payload over IPv4),
    // so the kernel refuses it whatever state its buffers are in.
    const std::vector<std::byte> too_big(70'000);
    EXPECT_FALSE(sender.send(too_big));
    EXPECT_EQ(sender.sent(), 0U);
    EXPECT_EQ(sender.dropped(), 1U);

    // The sender is not broken by it.
    const std::vector<std::byte> small(8, std::byte{1});
    EXPECT_TRUE(sender.send(small));
    EXPECT_EQ(sender.sent(), 1U);
    EXPECT_EQ(sender.dropped(), 1U);
}

using Mirror = book::BookManager<book::OrderStore, book::PriceLevels>;

// An engine publishing to a UDP address, and a subscriber rebuilding the book
// from what arrives there.
void expect_feed_round_trip(const std::string& host) {
    net::UdpReceiver socket(host, 0);
    net::UdpSender sender(host, socket.port());
    const net::MoldSession session = net::make_session("UDPTEST");

    gen::OrderFlow flow({.seed = 4, .symbols = 5, .target_live_orders = 200});
    net::MoldPacketizer packetizer(session, SendTo{&sender});
    using Engine = engine::ReferenceEngine<gen::OrderFlow, net::MoldPacketizer<SendTo>>;
    const auto engine = std::make_unique<Engine>(flow, packetizer);
    const auto mirror = std::make_unique<Mirror>();
    net::MoldReceiver receiver(session, *mirror);

    const auto drain = [&] {
        return receive_until(
            socket, [&](std::span<const std::byte> p) { receiver.on_packet(p); },
            [&] { return receiver.expected() == packetizer.next_sequence(); });
    };

    flow.open(*engine);
    packetizer.flush();
    ASSERT_TRUE(drain());
    // In small batches, read as they go, so the socket's buffer never fills:
    // this checks the plumbing, not what happens under loss.
    for (int batch = 0; batch < 200; ++batch) {
        for (int i = 0; i < 25; ++i) {
            gen::apply(*engine, flow.next());
        }
        packetizer.flush();
        ASSERT_TRUE(drain()) << "batch " << batch;
    }
    packetizer.end_of_session();
    ASSERT_TRUE(receive_until(
        socket, [&](std::span<const std::byte> p) { receiver.on_packet(p); },
        [&] { return receiver.stats().ended; }));

    EXPECT_EQ(sender.dropped(), 0U);
    EXPECT_TRUE(receiver.complete());
    EXPECT_EQ(receiver.stats().packets, packetizer.packets());
    EXPECT_TRUE(engine::compare_depth(*engine, *mirror).ok());
    EXPECT_EQ(mirror->counters(), book::Counters{});
}

TEST(UdpFeed, ASubscriberRebuildsTheBookOverLoopback) {
    expect_feed_round_trip("127.0.0.1");
}

TEST(UdpFeed, ASubscriberRebuildsTheBookOverMulticast) {
    // Multicast needs the loopback interface to allow it, which most machines
    // and containers do and some do not. Where it is not available this is
    // skipped, not failed: the code path differs from the test above only in
    // joining the group.
    const std::string group = "239.255.42.77";
    try {
        net::UdpReceiver probe(group, 0);
        net::UdpSender sender(group, probe.port());
        const std::vector<std::byte> ping{std::byte{1}};
        bool heard = false;
        if (sender.send(ping)) {
            static_cast<void>(receive_until(
                probe, [&](std::span<const std::byte>) { heard = true; }, [&] { return heard; }));
        }
        if (!heard) {
            GTEST_SKIP() << "multicast datagrams do not loop back on this machine";
        }
    } catch (const std::exception& e) {
        GTEST_SKIP() << "multicast is not available here: " << e.what();
    }
    expect_feed_round_trip(group);
}

}  // namespace
