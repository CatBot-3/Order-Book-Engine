#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "obe/book/bbo_hash.hpp"
#include "obe/book/book_manager.hpp"
#include "obe/book/types.hpp"
#include "obe/feed/parser.hpp"
#include "obe/gen/synthetic_feed.hpp"
#include "support/implementations.hpp"
#include "support/naive_book.hpp"
#include "support/printers.hpp"
#include "support/wire.hpp"

// End-to-end tests: bytes go in through the parser and the book that comes out
// is checked. They run against every store/levels pair in
// support/implementations.hpp.

namespace {

using namespace obe;
using book::Bbo;
using book::BboUpdate;
using book::Level;
using obe::test::Wire;

struct Recorder {
    std::vector<BboUpdate> updates;
    void on_bbo(const BboUpdate& u) { updates.push_back(u); }
};

template <class Levels>
std::vector<Level> walk(const Levels& levels) {
    std::vector<Level> out;
    levels.for_each([&out](const Level& level) {
        out.push_back(level);
        return true;
    });
    return out;
}

template <class Impl>
class BookReplay : public ::testing::Test {
 protected:
    template <class Listener>
    using Manager = book::BookManager<typename Impl::Store, typename Impl::Levels, Listener>;
};
TYPED_TEST_SUITE(BookReplay, test::BookTypes);

// --- Golden stream ----------------------------------------------------------------------
//
// A short day for one security, built byte by byte at the offsets in the
// Nasdaq specification, with the expected book worked out by hand.

constexpr std::uint16_t kLocate = 5;

struct GoldenStream {
    std::vector<std::byte> bytes;
    std::uint64_t t = 0;

    Wire message(char type, std::size_t size, std::uint16_t locate = kLocate) {
        t += 1'000;
        Wire w(type, size);
        w.header(locate, 0, t);
        return w;
    }
    void push(const Wire& w) { w.append_framed_to(bytes); }
};

GoldenStream golden() {
    GoldenStream s;
    s.push(s.message('S', 12, 0).ch(11, 'O'));                    // t=1000
    s.push(s.message('R', 39).text(11, "TEST    ").ch(19, 'Q'));  // t=2000
    s.push(s.message('H', 25).text(11, "TEST    ").ch(19, 'T'));  // t=3000
    // t=4000  A: order 1 buys 100 at 10.00
    s.push(s.message('A', 36)
               .u64(11, 1)
               .ch(19, 'B')
               .u32(20, 100)
               .text(24, "TEST    ")
               .u32(32, 100'000));
    // t=5000  A: order 2 sells 200 at 10.10
    s.push(s.message('A', 36)
               .u64(11, 2)
               .ch(19, 'S')
               .u32(20, 200)
               .text(24, "TEST    ")
               .u32(32, 101'000));
    // t=6000  F: order 3 sells 300 at 10.05, attributed
    s.push(s.message('F', 40)
               .u64(11, 3)
               .ch(19, 'S')
               .u32(20, 300)
               .text(24, "TEST    ")
               .u32(32, 100'500)
               .text(36, "MPID"));
    // t=7000  E: 100 of order 3 execute
    s.push(s.message('E', 31).u64(11, 3).u32(19, 100).u64(23, 9001));
    // t=8000  P: a hidden order trades 500 at 10.07. Not in the displayed book.
    s.push(s.message('P', 44)
               .u64(11, 0)
               .ch(19, 'B')
               .u32(20, 500)
               .text(24, "TEST    ")
               .u32(32, 100'700)
               .u64(36, 9002));
    // t=9000  C: 50 of order 2 execute at 10.10, non-printable
    s.push(s.message('C', 36).u64(11, 2).u32(19, 50).u64(23, 9003).ch(31, 'N').u32(32, 101'000));
    // t=10000 X: 40 shares of order 1 are cancelled
    s.push(s.message('X', 23).u64(11, 1).u32(19, 40));
    // t=11000 U: order 1 becomes order 4, 60 shares at 10.02
    s.push(s.message('U', 35).u64(11, 1).u64(19, 4).u32(27, 60).u32(31, 100'200));
    // t=12000 Q: a cross prints 10'000 shares. Not in the displayed book.
    s.push(s.message('Q', 40)
               .u64(11, 10'000)
               .text(19, "TEST    ")
               .u32(27, 100'300)
               .u64(31, 9004)
               .ch(39, 'O'));
    // t=13000 D: order 2 is deleted (150 shares were left)
    s.push(s.message('D', 19).u64(11, 2));
    s.push(s.message('S', 12, 0).ch(11, 'C'));  // t=14000
    return s;
}

TYPED_TEST(BookReplay, GoldenStreamProducesTheHandWorkedBook) {
    const GoldenStream stream = golden();
    typename TestFixture::template Manager<Recorder> manager;
    feed::ItchParser parser(manager);
    const feed::ParseResult result = parser.parse(stream.bytes);
    ASSERT_TRUE(result.ok()) << feed::to_string(result.status) << " at " << result.offset;
    ASSERT_EQ(result.messages, 14U);

    ASSERT_EQ(manager.find_locate("TEST"), kLocate);
    const auto& book = manager.book(kLocate);
    EXPECT_EQ(walk(book.bids()), (std::vector<Level>{{100'200, 60}}));
    EXPECT_EQ(walk(book.asks()), (std::vector<Level>{{100'500, 200}}));
    EXPECT_EQ(book.trading_state(), 'T');

    // Volume: the 100 from 'E'. Not the non-printable 'C', not 'P', not 'Q'.
    EXPECT_EQ(book.executed_shares(), 100U);

    // Orders 3 (200 left) and 4 are resting; 1 and 2 are gone.
    EXPECT_EQ(manager.orders().size(), 2U);
    EXPECT_EQ(manager.orders().find(1), nullptr);
    EXPECT_EQ(manager.orders().find(2), nullptr);
    ASSERT_NE(manager.orders().find(3), nullptr);
    EXPECT_EQ(*manager.orders().find(3), (book::OrderRecord{100'500, 200, Side::Sell}));
    ASSERT_NE(manager.orders().find(4), nullptr);
    EXPECT_EQ(*manager.orders().find(4), (book::OrderRecord{100'200, 60, Side::Buy}));

    const std::vector<BboUpdate> expected{
        {kLocate, 4'000, {100'000, 100, 0, 0}},          // A 1
        {kLocate, 5'000, {100'000, 100, 101'000, 200}},  // A 2
        {kLocate, 6'000, {100'000, 100, 100'500, 300}},  // F 3 becomes the best ask
        {kLocate, 7'000, {100'000, 100, 100'500, 200}},  // E on 3
                                                         // P: nothing
                                                         // C on 2: 10.10 is behind the best
        {kLocate, 10'000, {100'000, 60, 100'500, 200}},  // X on 1
        {kLocate, 11'000, {100'200, 60, 100'500, 200}},  // U 1 -> 4
                                                         // Q: nothing
                                                         // D on 2: behind the best
    };
    EXPECT_EQ(manager.listener().updates, expected);

    EXPECT_EQ(manager.counters(), book::Counters{});
    EXPECT_TRUE(manager.audit().clean());
    EXPECT_EQ(manager.stats().bbo_updates, expected.size());
}

TYPED_TEST(BookReplay, MessagesThatAreNotOrderMessagesNeverChangeTheBook) {
    typename TestFixture::template Manager<Recorder> manager;
    feed::ItchParser parser(manager);

    std::vector<std::byte> setup;
    Wire('A', 36)
        .header(kLocate, 0, 1)
        .u64(11, 1)
        .ch(19, 'B')
        .u32(20, 100)
        .u32(32, 100'000)
        .append_framed_to(setup);
    Wire('A', 36)
        .header(kLocate, 0, 2)
        .u64(11, 2)
        .ch(19, 'S')
        .u32(20, 100)
        .u32(32, 100'100)
        .append_framed_to(setup);
    ASSERT_TRUE(parser.parse(setup).ok());
    const Bbo before = manager.book(kLocate).bbo();
    const std::size_t updates_before = manager.listener().updates.size();

    // One of every type that is not A, F, E, C, X, D or U, all naming the same
    // security, and with an "order reference" field of 1 where one exists.
    std::vector<std::byte> noise;
    for (const char type :
         {'S', 'R', 'H', 'Y', 'L', 'V', 'W', 'K', 'J', 'h', 'P', 'Q', 'B', 'I', 'N', 'O'}) {
        Wire w(type, feed::message_size(type));
        w.header(kLocate, 0, 3);
        if (w.size() >= 19) {
            w.u64(11, 1);
        }
        w.append_framed_to(noise);
    }
    const feed::ParseResult result = parser.parse(noise);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.messages, 16U);

    EXPECT_EQ(manager.book(kLocate).bbo(), before);
    EXPECT_EQ(manager.listener().updates.size(), updates_before);
    EXPECT_EQ(manager.orders().size(), 2U);
    EXPECT_EQ(manager.book(kLocate).executed_shares(), 0U);
    EXPECT_EQ(manager.counters(), book::Counters{});
}

// --- Differential test against the oracle ---------------------------------------------------

TYPED_TEST(BookReplay, MatchesTheNaiveOracleUpdateForUpdateOnRandomFlow) {
    for (const std::uint64_t seed : {1ULL, 2ULL, 20261003ULL}) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        const gen::SyntheticConfig cfg{
            .seed = seed, .symbols = 6, .messages = 30'000, .target_live_orders = 300};
        const std::vector<std::byte> stream = gen::make_synthetic_feed(cfg);

        typename TestFixture::template Manager<Recorder> manager;
        feed::ItchParser book_parser(manager);
        ASSERT_TRUE(book_parser.parse(stream).ok());

        test::NaiveBook oracle;
        feed::ItchParser oracle_parser(oracle);
        ASSERT_TRUE(oracle_parser.parse(stream).ok());
        ASSERT_EQ(oracle.errors, 0U) << "the generator produced an inconsistent stream";

        // Success criterion 2, on synthetic flow.
        EXPECT_EQ(manager.counters(), book::Counters{});
        const book::Audit audit = manager.audit();
        EXPECT_EQ(audit.empty_levels, 0U);
        EXPECT_EQ(audit.level_shares, audit.open_shares);
        EXPECT_EQ(audit.open_orders, oracle.open_orders());

        // Every best-bid-and-offer change, in order, with its timestamp.
        const std::vector<BboUpdate>& got = manager.listener().updates;
        ASSERT_EQ(got.size(), oracle.updates.size());
        ASSERT_GT(got.size(), 1'000U) << "the stream should exercise the top of the book";
        for (std::size_t i = 0; i < got.size(); ++i) {
            ASSERT_EQ(got[i], oracle.updates[i]) << "update " << i;
        }

        // Full depth at the end, not only the top.
        for (std::uint32_t locate = 1; locate <= cfg.symbols; ++locate) {
            const auto& book = manager.book(static_cast<Locate>(locate));
            EXPECT_EQ(walk(book.bids()), oracle.levels(static_cast<Locate>(locate), Side::Buy))
                << "bids of locate " << locate;
            EXPECT_EQ(walk(book.asks()), oracle.levels(static_cast<Locate>(locate), Side::Sell))
                << "asks of locate " << locate;
        }
    }
}

// --- Determinism and the hash -----------------------------------------------------------------

TYPED_TEST(BookReplay, TheSameInputGivesTheSameHashAndADifferentInputDoesNot) {
    const auto replay = [](std::uint64_t seed) {
        const std::vector<std::byte> stream = gen::make_synthetic_feed(
            {.seed = seed, .symbols = 10, .messages = 20'000, .target_live_orders = 500});
        typename TestFixture::template Manager<book::BboHasher> manager;
        feed::ItchParser parser(manager);
        EXPECT_TRUE(parser.parse(stream).ok());
        EXPECT_EQ(manager.listener().total_events(), manager.stats().bbo_updates);
        return manager.listener().combined();
    };
    const std::uint64_t first = replay(77);
    EXPECT_EQ(first, replay(77));
    EXPECT_NE(first, replay(78));
}

TYPED_TEST(BookReplay, TheHashAgreesWithTheRecordedUpdateStream) {
    // The hasher sees the same updates as a recorder would. Feeding the
    // recorded updates to a fresh hasher must give the same per-security hash.
    const std::vector<std::byte> stream = gen::make_synthetic_feed(
        {.seed = 5, .symbols = 4, .messages = 10'000, .target_live_orders = 200});

    typename TestFixture::template Manager<Recorder> recorded;
    feed::ItchParser recorded_parser(recorded);
    ASSERT_TRUE(recorded_parser.parse(stream).ok());

    typename TestFixture::template Manager<book::BboHasher> hashed;
    feed::ItchParser hashed_parser(hashed);
    ASSERT_TRUE(hashed_parser.parse(stream).ok());

    book::BboHasher replayed;
    for (const BboUpdate& u : recorded.listener().updates) {
        replayed.on_bbo(u);
    }
    EXPECT_EQ(replayed.combined(), hashed.listener().combined());
    for (std::uint32_t locate = 0; locate <= 5; ++locate) {
        EXPECT_EQ(replayed.hash(static_cast<Locate>(locate)),
                  hashed.listener().hash(static_cast<Locate>(locate)));
    }
}

// --- The assumption book_view relies on --------------------------------------------------------

TYPED_TEST(BookReplay, ReplayingOnlyOneSecuritysMessagesGivesThatSecuritysBook) {
    const std::vector<std::byte> stream = gen::make_synthetic_feed(
        {.seed = 9, .symbols = 5, .messages = 20'000, .target_live_orders = 400});
    constexpr Locate kWanted = 3;

    typename TestFixture::template Manager<Recorder> full;
    feed::ItchParser full_parser(full);
    ASSERT_TRUE(full_parser.parse(stream).ok());

    typename TestFixture::template Manager<Recorder> filtered;
    feed::ItchParser filtered_parser(filtered);
    feed::FrameReader reader(stream);
    feed::Frame frame;
    while (!reader.done()) {
        ASSERT_EQ(reader.next(frame), feed::ParseStatus::Ok);
        if (feed::peek_locate(frame) == kWanted) {
            ASSERT_EQ(filtered_parser.dispatch(frame), feed::ParseStatus::Ok);
        }
    }

    EXPECT_EQ(walk(filtered.book(kWanted).bids()), walk(full.book(kWanted).bids()));
    EXPECT_EQ(walk(filtered.book(kWanted).asks()), walk(full.book(kWanted).asks()));
    EXPECT_EQ(filtered.counters(), book::Counters{});

    std::vector<BboUpdate> wanted_updates;
    for (const BboUpdate& u : full.listener().updates) {
        if (u.locate == kWanted) {
            wanted_updates.push_back(u);
        }
    }
    EXPECT_EQ(filtered.listener().updates, wanted_updates);
}

}  // namespace
