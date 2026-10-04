#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/engine/reference_engine.hpp"
#include "obe/engine/round_trip.hpp"
#include "obe/feed/codec.hpp"
#include "obe/feed/handler.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/gen/rng.hpp"
#include "obe/net/moldudp.hpp"
#include "support/printers.hpp"
#include "support/trace_handler.hpp"
#include "support/wire.hpp"

// The MoldUDP64-style framing: the packetizer that builds packets and the
// receiver that checks their sequence. No sockets here; packets are kept in a
// vector, which makes it easy to lose, repeat and reorder them on purpose.

namespace {

using namespace obe;
namespace md = engine::md;

using Packet = std::vector<std::byte>;

// Keeps every packet the packetizer sends.
struct Keep {
    std::vector<Packet>* packets;
    void operator()(std::span<const std::byte> packet) const {
        packets->emplace_back(packet.begin(), packet.end());
    }
};

const net::MoldSession kSession = net::make_session("TEST");
const feed::Symbol kSymbol = feed::Symbol::from("ACME");

feed::AddOrder add(OrderId ref) {
    return md::add(1, ref, ref, Side::Buy, 100, kSymbol, 1'000'000);
}

std::uint64_t sequence_of(const Packet& p) {
    return feed::load_be<std::uint64_t>(p.data() + 10);
}
std::uint16_t count_of(const Packet& p) {
    return feed::load_be<std::uint16_t>(p.data() + 18);
}

// `packets` packets of `per_packet` Add Orders each, refs counting from 1.
std::vector<Packet> make_packets(std::size_t packets, std::size_t per_packet) {
    std::vector<Packet> out;
    net::MoldPacketizer packetizer(kSession, Keep{&out});
    OrderId ref = 1;
    for (std::size_t p = 0; p < packets; ++p) {
        for (std::size_t i = 0; i < per_packet; ++i) {
            packetizer.on_add(add(ref++));
        }
        packetizer.flush();
    }
    return out;
}

// The refs of the Add Orders a receiver delivered, in order.
struct Refs : feed::HandlerBase {
    std::vector<OrderId> seen;
    void on_add(const feed::AddOrder& m) { seen.push_back(m.order_ref); }
};

// --- Session names -----------------------------------------------------------

TEST(MoldSession, IsLeftJustifiedAndPaddedWithSpaces) {
    EXPECT_EQ(std::string(net::make_session("OBE").data(), 10), "OBE       ");
    EXPECT_EQ(std::string(net::make_session("").data(), 10), "          ");
    EXPECT_EQ(std::string(net::make_session("0123456789ABC").data(), 10), "0123456789");
}

// --- Packetizer --------------------------------------------------------------

TEST(MoldPacketizer, APacketIsHeaderThenLengthPrefixedMessages) {
    std::vector<Packet> sent;
    net::MoldPacketizer packetizer(kSession, Keep{&sent});
    const feed::AddOrder first = add(7);
    const feed::OrderDelete second = md::remove(1, 8, 7);
    packetizer.on_add(first);
    packetizer.on_delete(second);
    EXPECT_TRUE(sent.empty()) << "nothing is sent until the packet is flushed or full";
    packetizer.flush();
    ASSERT_EQ(sent.size(), 1U);

    // Built by hand, byte by byte, from the layout in the header's comment.
    Packet expected;
    for (const char c : std::string("TEST      ")) {
        expected.push_back(static_cast<std::byte>(c));
    }
    for (const int b : {0, 0, 0, 0, 0, 0, 0, 1}) {  // sequence 1
        expected.push_back(static_cast<std::byte>(b));
    }
    expected.push_back(std::byte{0});  // count 2
    expected.push_back(std::byte{2});
    Packet body(36);
    feed::encode(first, body.data());
    expected.push_back(std::byte{0});
    expected.push_back(std::byte{36});
    expected.insert(expected.end(), body.begin(), body.end());
    body.resize(19);
    feed::encode(second, body.data());
    expected.push_back(std::byte{0});
    expected.push_back(std::byte{19});
    expected.insert(expected.end(), body.begin(), body.end());

    EXPECT_EQ(sent[0], expected);
    EXPECT_EQ(sent[0].size(), 20U + 2 + 36 + 2 + 19);
}

TEST(MoldPacketizer, EachPacketCarriesTheNumberOfItsFirstMessage) {
    const std::vector<Packet> packets = make_packets(4, 3);
    ASSERT_EQ(packets.size(), 4U);
    EXPECT_EQ(sequence_of(packets[0]), 1U);
    EXPECT_EQ(sequence_of(packets[1]), 4U);
    EXPECT_EQ(sequence_of(packets[2]), 7U);
    EXPECT_EQ(sequence_of(packets[3]), 10U);
    for (const Packet& p : packets) {
        EXPECT_EQ(count_of(p), 3U);
    }
}

TEST(MoldPacketizer, FlushWithNothingPendingSendsNothing) {
    std::vector<Packet> sent;
    net::MoldPacketizer packetizer(kSession, Keep{&sent});
    packetizer.flush();
    packetizer.flush();
    EXPECT_TRUE(sent.empty());
    EXPECT_EQ(packetizer.packets(), 0U);
    EXPECT_EQ(packetizer.next_sequence(), 1U);
}

TEST(MoldPacketizer, AFullPacketGoesOutByItself) {
    std::vector<Packet> sent;
    constexpr std::size_t kMax = 200;
    net::MoldPacketizer packetizer(kSession, Keep{&sent}, kMax);
    for (OrderId ref = 1; ref <= 50; ++ref) {
        packetizer.on_add(add(ref));  // 38 bytes each with its length prefix
    }
    packetizer.flush();

    // (200 - 20) / 38 = 4 messages fit.
    std::uint64_t messages = 0;
    for (const Packet& p : sent) {
        EXPECT_LE(p.size(), kMax);
        EXPECT_EQ(sequence_of(p), messages + 1);
        messages += count_of(p);
    }
    EXPECT_EQ(messages, 50U);
    EXPECT_EQ(sent.size(), 13U);
    EXPECT_EQ(count_of(sent.front()), 4U);
    EXPECT_EQ(count_of(sent.back()), 2U);
    EXPECT_EQ(packetizer.messages(), 50U);
    EXPECT_EQ(packetizer.packets(), 13U);
    EXPECT_EQ(packetizer.next_sequence(), 51U);
}

TEST(MoldPacketizer, ALimitTooSmallForOneMessageIsRaised) {
    std::vector<Packet> sent;
    net::MoldPacketizer packetizer(kSession, Keep{&sent}, 1);
    packetizer.on_stock_directory(md::directory(1, kSymbol, 1));
    packetizer.on_add(add(1));
    packetizer.flush();
    std::uint64_t messages = 0;
    for (const Packet& p : sent) {
        messages += count_of(p);
    }
    EXPECT_EQ(messages, 2U);
}

TEST(MoldPacketizer, AHeartbeatStatesTheNextSequenceAndCarriesNothing) {
    std::vector<Packet> sent;
    net::MoldPacketizer packetizer(kSession, Keep{&sent});
    packetizer.heartbeat();
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].size(), net::kMoldHeaderSize);
    EXPECT_EQ(sequence_of(sent[0]), 1U);
    EXPECT_EQ(count_of(sent[0]), 0U);

    packetizer.on_add(add(1));
    packetizer.on_add(add(2));
    packetizer.heartbeat();  // sends the two pending messages first
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(count_of(sent[1]), 2U);
    EXPECT_EQ(sequence_of(sent[2]), 3U);
    EXPECT_EQ(count_of(sent[2]), 0U);
    EXPECT_EQ(packetizer.next_sequence(), 3U) << "a heartbeat uses no sequence number";
}

TEST(MoldPacketizer, EndOfSessionIsMarkedInTheCount) {
    std::vector<Packet> sent;
    net::MoldPacketizer packetizer(kSession, Keep{&sent});
    packetizer.on_add(add(1));
    packetizer.end_of_session();
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(count_of(sent[0]), 1U);
    EXPECT_EQ(count_of(sent[1]), net::kMoldEndOfSession);
    EXPECT_EQ(sequence_of(sent[1]), 2U);
    EXPECT_EQ(sent[1].size(), net::kMoldHeaderSize);
}

// --- Receiver ----------------------------------------------------------------

TEST(MoldReceiver, DeliversACompleteStreamInOrder) {
    const std::vector<Packet> packets = make_packets(5, 4);
    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    for (const Packet& p : packets) {
        ASSERT_TRUE(receiver.on_packet(p));
    }
    ASSERT_EQ(refs.seen.size(), 20U);
    for (std::size_t i = 0; i < refs.seen.size(); ++i) {
        EXPECT_EQ(refs.seen[i], i + 1);
    }
    EXPECT_TRUE(receiver.complete());
    EXPECT_EQ(receiver.expected(), 21U);
    EXPECT_EQ(receiver.stats(), (net::MoldStats{.packets = 5, .messages = 20}));
}

TEST(MoldReceiver, ALostPacketIsAGapOfExactlyItsMessages) {
    const std::vector<Packet> packets = make_packets(5, 4);
    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    for (const std::size_t i : {0U, 1U, 3U, 4U}) {  // packet 2 never arrives
        ASSERT_TRUE(receiver.on_packet(packets[i]));
    }
    EXPECT_FALSE(receiver.complete());
    EXPECT_EQ(receiver.stats().gaps, 1U);
    EXPECT_EQ(receiver.stats().missed_messages, 4U);
    EXPECT_EQ(receiver.stats().messages, 16U);
    // What did arrive is still delivered, in order, each once.
    EXPECT_EQ(refs.seen,
              (std::vector<OrderId>{1, 2, 3, 4, 5, 6, 7, 8, 13, 14, 15, 16, 17, 18, 19, 20}));
    EXPECT_EQ(receiver.expected(), 21U);
}

TEST(MoldReceiver, LosingTheFirstPacketIsAGapToo) {
    const std::vector<Packet> packets = make_packets(3, 2);
    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    ASSERT_TRUE(receiver.on_packet(packets[1]));
    EXPECT_EQ(receiver.stats().gaps, 1U);
    EXPECT_EQ(receiver.stats().missed_messages, 2U);
}

TEST(MoldReceiver, ARepeatedPacketIsDeliveredOnce) {
    const std::vector<Packet> packets = make_packets(3, 4);
    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    ASSERT_TRUE(receiver.on_packet(packets[0]));
    ASSERT_TRUE(receiver.on_packet(packets[1]));
    ASSERT_TRUE(receiver.on_packet(packets[1]));
    ASSERT_TRUE(receiver.on_packet(packets[0]));
    ASSERT_TRUE(receiver.on_packet(packets[2]));
    EXPECT_EQ(refs.seen.size(), 12U);
    EXPECT_TRUE(receiver.complete());
    EXPECT_EQ(receiver.stats().duplicates, 8U);
    EXPECT_EQ(receiver.stats().gaps, 0U);
}

TEST(MoldReceiver, APacketThatArrivesLateIsTooLate) {
    // 1, 3, 2: by the time packet 2 arrives the receiver has moved past it.
    // Its messages were counted as missed and are now skipped as old: handing
    // them over after packet 3's would deliver the stream out of order.
    const std::vector<Packet> packets = make_packets(3, 4);
    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    ASSERT_TRUE(receiver.on_packet(packets[0]));
    ASSERT_TRUE(receiver.on_packet(packets[2]));
    ASSERT_TRUE(receiver.on_packet(packets[1]));
    EXPECT_EQ(refs.seen, (std::vector<OrderId>{1, 2, 3, 4, 9, 10, 11, 12}));
    EXPECT_EQ(receiver.stats().gaps, 1U);
    EXPECT_EQ(receiver.stats().missed_messages, 4U);
    EXPECT_EQ(receiver.stats().duplicates, 4U);
    EXPECT_FALSE(receiver.complete());
}

TEST(MoldReceiver, APacketThatOverlapsDeliversOnlyItsNewPart) {
    // The same six messages cut two ways: 3 + 3, and 2 + 4.
    std::vector<Packet> by_three;
    std::vector<Packet> uneven;
    {
        net::MoldPacketizer a(kSession, Keep{&by_three});
        net::MoldPacketizer b(kSession, Keep{&uneven});
        for (OrderId ref = 1; ref <= 6; ++ref) {
            a.on_add(add(ref));
            b.on_add(add(ref));
            if (ref == 3) {
                a.flush();
            }
            if (ref == 2) {
                b.flush();
            }
        }
        a.flush();
        b.flush();
    }
    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    ASSERT_TRUE(receiver.on_packet(by_three[0]));  // 1 2 3
    ASSERT_TRUE(receiver.on_packet(uneven[1]));    // 3 4 5 6: only 4 5 6 are new
    EXPECT_EQ(refs.seen, (std::vector<OrderId>{1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(receiver.stats().duplicates, 1U);
    EXPECT_TRUE(receiver.complete());
}

TEST(MoldReceiver, AHeartbeatRevealsALossAtTheEndOfABurst) {
    std::vector<Packet> sent;
    net::MoldPacketizer packetizer(kSession, Keep{&sent});
    packetizer.on_add(add(1));
    packetizer.flush();
    packetizer.on_add(add(2));
    packetizer.on_add(add(3));
    packetizer.flush();
    packetizer.heartbeat();
    ASSERT_EQ(sent.size(), 3U);

    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    ASSERT_TRUE(receiver.on_packet(sent[0]));
    // sent[1] is lost. With no later message, only the heartbeat can say so.
    EXPECT_TRUE(receiver.complete());
    ASSERT_TRUE(receiver.on_packet(sent[2]));
    EXPECT_FALSE(receiver.complete());
    EXPECT_EQ(receiver.stats().missed_messages, 2U);
    EXPECT_EQ(receiver.stats().heartbeats, 1U);

    // A heartbeat that agrees with the receiver changes nothing.
    Refs refs2;
    net::MoldReceiver whole(kSession, refs2);
    for (const Packet& p : sent) {
        ASSERT_TRUE(whole.on_packet(p));
    }
    EXPECT_TRUE(whole.complete());
    EXPECT_EQ(whole.stats().heartbeats, 1U);
}

TEST(MoldReceiver, ALossIsCountedOnceHoweverOftenItIsNoticed) {
    std::vector<Packet> sent;
    net::MoldPacketizer packetizer(kSession, Keep{&sent});
    packetizer.on_add(add(1));
    packetizer.flush();
    packetizer.on_add(add(2));
    packetizer.on_add(add(3));
    packetizer.flush();
    packetizer.heartbeat();
    packetizer.on_add(add(4));
    packetizer.flush();
    packetizer.heartbeat();
    ASSERT_EQ(sent.size(), 5U);

    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    ASSERT_TRUE(receiver.on_packet(sent[0]));
    // sent[1] is lost. The heartbeat says so first...
    ASSERT_TRUE(receiver.on_packet(sent[2]));
    EXPECT_EQ(receiver.expected(), 4U);
    EXPECT_EQ(receiver.stats().gaps, 1U);
    EXPECT_EQ(receiver.stats().missed_messages, 2U);
    // ...and the next data packet and the next heartbeat only repeat it.
    ASSERT_TRUE(receiver.on_packet(sent[3]));
    ASSERT_TRUE(receiver.on_packet(sent[4]));
    EXPECT_EQ(receiver.expected(), 5U);
    EXPECT_EQ(receiver.stats().gaps, 1U);
    EXPECT_EQ(receiver.stats().missed_messages, 2U);
    EXPECT_EQ(refs.seen, (std::vector<OrderId>{1, 4}));
}

TEST(MoldReceiver, SeesTheEndOfTheSession) {
    std::vector<Packet> sent;
    net::MoldPacketizer packetizer(kSession, Keep{&sent});
    packetizer.on_add(add(1));
    packetizer.end_of_session();
    ASSERT_EQ(sent.size(), 2U);
    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    ASSERT_TRUE(receiver.on_packet(sent[0]));
    EXPECT_FALSE(receiver.stats().ended);
    ASSERT_TRUE(receiver.on_packet(sent[1]));
    EXPECT_TRUE(receiver.stats().ended);
    EXPECT_TRUE(receiver.complete());

    // If the last data packet was lost, the end marker shows it.
    Refs refs2;
    net::MoldReceiver late(kSession, refs2);
    ASSERT_TRUE(late.on_packet(sent[1]));
    EXPECT_TRUE(late.stats().ended);
    EXPECT_EQ(late.stats().missed_messages, 1U);
}

TEST(MoldReceiver, IgnoresPacketsOfAnotherSession) {
    std::vector<Packet> other;
    net::MoldPacketizer packetizer(net::make_session("OTHER"), Keep{&other});
    packetizer.on_add(add(1));
    packetizer.flush();
    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    EXPECT_FALSE(receiver.on_packet(other[0]));
    EXPECT_TRUE(refs.seen.empty());
    EXPECT_EQ(receiver.stats().other_sessions, 1U);
    EXPECT_EQ(receiver.stats().packets, 0U);
    EXPECT_EQ(receiver.expected(), 1U);

    // The whole name is compared: one that differs only in its last
    // character, or only by being longer, is another session too.
    std::uint64_t refused = 1;
    for (const char* name : {"TEST     2", "TEST2", "TES"}) {
        std::vector<Packet> near;
        net::MoldPacketizer close(net::make_session(name), Keep{&near});
        close.on_add(add(1));
        close.flush();
        EXPECT_FALSE(receiver.on_packet(near[0])) << "'" << name << "'";
        EXPECT_EQ(receiver.stats().other_sessions, ++refused);
    }
    EXPECT_TRUE(refs.seen.empty());
}

TEST(MoldReceiver, RejectsPacketsThatAreCutShort) {
    const Packet whole = make_packets(1, 3)[0];
    // Every possible truncation: none may be read past its end. Anything that
    // loses part of a message block is malformed; the messages before the cut
    // are still delivered.
    for (std::size_t size = 0; size < whole.size(); ++size) {
        Refs refs;
        net::MoldReceiver receiver(kSession, refs);
        const bool ok = receiver.on_packet({whole.data(), size});
        EXPECT_FALSE(ok) << "size " << size;
        EXPECT_EQ(receiver.stats().bad_packets, 1U) << "size " << size;
        const std::size_t complete_blocks = size < 20 ? 0 : (size - 20) / 38;
        EXPECT_EQ(refs.seen.size(), complete_blocks) << "size " << size;
    }
    Refs refs;
    net::MoldReceiver receiver(kSession, refs);
    EXPECT_TRUE(receiver.on_packet(whole));
    EXPECT_EQ(refs.seen.size(), 3U);
}

TEST(MoldReceiver, ABlockThatIsNotAnItchMessageIsCountedAndSkipped) {
    // A hand-built packet: one good Add Order, one block of an unknown type,
    // one good Delete.
    Packet packet;
    for (const char c : std::string("TEST      ")) {
        packet.push_back(static_cast<std::byte>(c));
    }
    for (const int b : {0, 0, 0, 0, 0, 0, 0, 1, 0, 3}) {  // sequence 1, count 3
        packet.push_back(static_cast<std::byte>(b));
    }
    const auto block = [&packet](const Packet& body) {
        packet.push_back(static_cast<std::byte>(body.size() >> 8));
        packet.push_back(static_cast<std::byte>(body.size() & 0xff));
        packet.insert(packet.end(), body.begin(), body.end());
    };
    Packet body(36);
    feed::encode(add(5), body.data());
    block(body);
    block(Packet{static_cast<std::byte>('?'), std::byte{1}, std::byte{2}});
    body.resize(19);
    feed::encode(md::remove(1, 9, 5), body.data());
    block(body);

    test::TraceHandler trace;
    net::MoldReceiver receiver(kSession, trace);
    EXPECT_TRUE(receiver.on_packet(packet));
    EXPECT_EQ(trace.trace, "AD");
    EXPECT_EQ(receiver.stats().bad_messages, 1U);
    EXPECT_EQ(receiver.stats().messages, 2U);
    // It still had a sequence number, so nothing was "missed".
    EXPECT_EQ(receiver.expected(), 4U);
    EXPECT_TRUE(receiver.complete());
}

// --- The whole way round -----------------------------------------------------

using Mirror = book::BookManager<book::OrderStore, book::PriceLevels>;

// An engine that publishes straight into packets, with everything it refers
// to kept alive beside it.
struct Published {
    using Packetizer = net::MoldPacketizer<Keep>;
    using Engine = engine::ReferenceEngine<gen::OrderFlow, Packetizer>;

    std::vector<Packet> packets;
    gen::OrderFlow flow;
    Packetizer packetizer;
    std::unique_ptr<Engine> engine;

    explicit Published(std::uint64_t seed)
        : flow({.seed = seed, .symbols = 6, .target_live_orders = 300}),
          packetizer(kSession, Keep{&packets}, 300),
          engine(std::make_unique<Engine>(flow, packetizer)) {}
};

// Runs seeded flow through the engine, flushing every few requests as the
// gateway does every turn.
std::unique_ptr<Published> publish(std::uint64_t seed, int commands) {
    auto out = std::make_unique<Published>(seed);
    out->flow.open(*out->engine);
    for (int i = 0; i < commands; ++i) {
        gen::apply(*out->engine, out->flow.next());
        if (i % 3 == 0) {
            out->packetizer.flush();
        }
    }
    out->packetizer.end_of_session();
    return out;
}

TEST(MoldRoundTrip, ASubscriberRebuildsTheEnginesBookFromThePackets) {
    const std::unique_ptr<Published> published = publish(1, 20'000);
    ASSERT_GT(published->packets.size(), 5'000U);

    const auto mirror = std::make_unique<Mirror>();
    net::MoldReceiver receiver(kSession, *mirror);
    for (const Packet& p : published->packets) {
        ASSERT_TRUE(receiver.on_packet(p));
    }
    EXPECT_TRUE(receiver.complete());
    EXPECT_TRUE(receiver.stats().ended);
    EXPECT_TRUE(engine::compare_depth(*published->engine, *mirror).ok());
    EXPECT_EQ(mirror->counters(), book::Counters{});
    EXPECT_EQ(mirror->orders().size(), published->engine->open_orders());
}

TEST(MoldRoundTrip, DuplicatesAndRepeatsDoNotDisturbTheBook) {
    const std::unique_ptr<Published> published = publish(2, 10'000);
    const auto mirror = std::make_unique<Mirror>();
    net::MoldReceiver receiver(kSession, *mirror);
    gen::SplitMix64 rng(7);
    for (std::size_t i = 0; i < published->packets.size(); ++i) {
        ASSERT_TRUE(receiver.on_packet(published->packets[i]));
        // Now and then the network delivers something again.
        if (rng.below(4) == 0) {
            const std::size_t again = i - std::min<std::size_t>(i, rng.below(5));
            ASSERT_TRUE(receiver.on_packet(published->packets[again]));
        }
    }
    EXPECT_TRUE(receiver.complete());
    EXPECT_GT(receiver.stats().duplicates, 100U);
    EXPECT_TRUE(engine::compare_depth(*published->engine, *mirror).ok());
    EXPECT_EQ(mirror->counters(), book::Counters{});
}

TEST(MoldRoundTrip, ALossIsAlwaysReportedNeverSilent) {
    // Drop one packet in fifty. Whatever that does to the book, the receiver
    // must know it happened: complete() is the subscriber's only way to tell
    // a trustworthy book from a wrong one.
    const std::unique_ptr<Published> published = publish(3, 10'000);
    for (std::uint64_t trial = 1; trial <= 20; ++trial) {
        const auto mirror = std::make_unique<Mirror>();
        net::MoldReceiver receiver(kSession, *mirror);
        gen::SplitMix64 rng(trial);
        std::uint64_t dropped_messages = 0;
        for (const Packet& p : published->packets) {
            if (count_of(p) != 0 && count_of(p) != net::kMoldEndOfSession && rng.below(50) == 0) {
                dropped_messages += count_of(p);
                continue;
            }
            ASSERT_TRUE(receiver.on_packet(p));
        }
        ASSERT_GT(dropped_messages, 0U);
        EXPECT_FALSE(receiver.complete()) << "trial " << trial;
        EXPECT_EQ(receiver.stats().missed_messages, dropped_messages) << "trial " << trial;
    }
}

}  // namespace
