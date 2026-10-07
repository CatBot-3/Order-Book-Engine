#include "obe/store/tick_profile.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/feed/parser.hpp"
#include "support/flow_run.hpp"
#include "support/tick_streams.hpp"

namespace {

using namespace obe;
using store::FieldChanges;
using store::Magnitudes;
using store::Tick;
using store::TickProfile;

static_assert(Magnitudes::class_of(0) == 0);
static_assert(Magnitudes::class_of(1) == 1);
static_assert(Magnitudes::class_of(127) == 1);
static_assert(Magnitudes::class_of(128) == 2);

TEST(Magnitudes, TheClassesAreWhatOneMoreByteOfAVarintHolds) {
    EXPECT_EQ(Magnitudes::class_of(0), 0U);
    EXPECT_EQ(Magnitudes::class_of(1), 1U);
    EXPECT_EQ(Magnitudes::class_of(127), 1U);
    EXPECT_EQ(Magnitudes::class_of(128), 2U);
    EXPECT_EQ(Magnitudes::class_of(16'383), 2U);
    EXPECT_EQ(Magnitudes::class_of(16'384), 3U);
    EXPECT_EQ(Magnitudes::class_of(2'097'151), 3U);
    EXPECT_EQ(Magnitudes::class_of(2'097'152), 4U);
    EXPECT_EQ(Magnitudes::class_of(268'435'455), 4U);
    EXPECT_EQ(Magnitudes::class_of(268'435'456), 5U);
    EXPECT_EQ(Magnitudes::class_of(std::numeric_limits<std::uint64_t>::max()), 5U);

    Magnitudes m;
    for (const std::uint64_t v : {0ULL, 0ULL, 5ULL, 200ULL, 1'000'000'000'000ULL}) {
        m.add(v);
    }
    EXPECT_EQ(m.count, (std::array<std::uint64_t, 6>{2, 1, 1, 0, 0, 1}));
    EXPECT_EQ(m.total(), 5U);
}

Tick tick(Nanos t, Locate locate, Price bid, std::uint64_t bid_qty, Price ask,
          std::uint64_t ask_qty) {
    return {locate, t, {bid, bid_qty, ask, ask_qty}};
}

// Seven ticks, each chosen to land in a different place in the tables. The
// comments say what each one is to the profile.
TEST(TickProfile, CountsWhatConsecutiveTicksDifferBy) {
    TickProfile p;
    // 1: the first tick of security 7. Nothing to compare with.
    p.add(tick(1'000, 7, 1'000'000, 300, 1'000'100, 500));
    // 2: same time, same security, the bid size down one round lot.
    p.add(tick(1'000, 7, 1'000'000, 200, 1'000'100, 500));
    // 3: 500 ns on, the first tick of security 9.
    p.add(tick(1'500, 9, 250'000, 100, 250'300, 100));
    // 4: time 100 ns BACK, security 7 again after two ticks, the ask up a
    //    cent and its size up three round lots.
    p.add(tick(1'400, 7, 1'000'000, 200, 1'000'200, 800));
    // 5: a long gap, the bid side of 7 empties.
    p.add(tick(3'000'000, 7, 0, 0, 1'000'200, 800));
    // 6: same time, security 9 three ticks after its last, and nothing changed.
    p.add(tick(3'000'000, 9, 250'000, 100, 250'300, 100));
    // 7: 1 ns on, its bid up half a cent.
    p.add(tick(3'000'001, 9, 250'050, 100, 250'300, 100));

    EXPECT_EQ(p.ticks(), 7U);
    EXPECT_EQ(p.securities(), 2U);

    // Gaps: 0, 500, 100 (backwards), 2,998,600, 0, 1.
    EXPECT_EQ(p.time_gap().count, (std::array<std::uint64_t, 6>{2, 2, 1, 0, 1, 0}));
    EXPECT_EQ(p.time_backwards(), 1U);
    EXPECT_EQ(p.same_security(), 3U) << "ticks 2, 5 and 7";

    // Ticks since the same security: 1, 2, 1, 3, 1.
    EXPECT_EQ(p.ticks_since().count, (std::array<std::uint64_t, 6>{0, 5, 0, 0, 0, 0}));

    std::array<std::uint64_t, 16> masks{};
    masks[0] = 1;                                              // tick 6
    masks[TickProfile::kBidQty] = 1;                           // tick 2
    masks[TickProfile::kAskPrice | TickProfile::kAskQty] = 1;  // tick 4
    masks[TickProfile::kBidPrice | TickProfile::kBidQty] = 1;  // tick 5
    masks[TickProfile::kBidPrice] = 1;                         // tick 7
    EXPECT_EQ(p.masks(), masks);

    FieldChanges bid_price;  // 1,000,000 -> 0 and 250,000 -> 250,050
    bid_price.changes = 2;
    bid_price.down = 1;
    bid_price.to_or_from_zero = 1;
    bid_price.hundreds = 1;
    bid_price.size.count = {0, 1, 0, 1, 0, 0};
    EXPECT_EQ(p.bid_price(), bid_price);

    FieldChanges bid_qty;  // 300 -> 200 and 200 -> 0
    bid_qty.changes = 2;
    bid_qty.down = 2;
    bid_qty.to_or_from_zero = 1;
    bid_qty.hundreds = 2;
    bid_qty.one_hundred = 1;
    bid_qty.size.count = {0, 1, 1, 0, 0, 0};
    EXPECT_EQ(p.bid_qty(), bid_qty);

    FieldChanges ask_price;  // 1,000,100 -> 1,000,200
    ask_price.changes = 1;
    ask_price.hundreds = 1;
    ask_price.one_hundred = 1;
    ask_price.size.count = {0, 1, 0, 0, 0, 0};
    EXPECT_EQ(p.ask_price(), ask_price);

    FieldChanges ask_qty;  // 500 -> 800
    ask_qty.changes = 1;
    ask_qty.hundreds = 1;
    ask_qty.size.count = {0, 0, 1, 0, 0, 0};
    EXPECT_EQ(p.ask_qty(), ask_qty);
}

// How long ago a security last ticked is counted in ticks back from now, not
// from the start of the stream.
TEST(TickProfile, TheDistanceToASecuritysLastTickIsCountedBackFromNow) {
    TickProfile p;
    p.add(tick(1, 1, 100, 1, 200, 1));
    // Three hundred ticks of another security: each is one tick after its
    // own last, however far into the stream it comes.
    for (std::uint64_t i = 0; i < 300; ++i) {
        p.add(tick(2 + i, 2, 100, 1 + i, 200, 1));
    }
    EXPECT_EQ(p.ticks_since().count, (std::array<std::uint64_t, 6>{0, 299, 0, 0, 0, 0}));
    // And the first security again, 301 ticks after its last.
    p.add(tick(500, 1, 100, 2, 200, 1));
    EXPECT_EQ(p.ticks_since().count, (std::array<std::uint64_t, 6>{0, 299, 1, 0, 0, 0}));
    EXPECT_EQ(p.same_security(), 299U);
}

// A side appearing counts with a side emptying: both are the jumps between
// nothing and a whole price that a difference handles worst.
TEST(TickProfile, ASideAppearingCountsAsMuchAsOneEmptying) {
    TickProfile p;
    p.add(tick(1, 3, 0, 0, 500'000, 100));          // no bid yet
    p.add(tick(2, 3, 499'900, 300, 500'000, 100));  // the bid appears
    p.add(tick(3, 3, 499'900, 300, 0, 0));          // the ask empties
    EXPECT_EQ(p.bid_price().changes, 1U);
    EXPECT_EQ(p.bid_price().to_or_from_zero, 1U);
    EXPECT_EQ(p.bid_price().down, 0U);
    EXPECT_EQ(p.bid_qty().to_or_from_zero, 1U);
    EXPECT_EQ(p.ask_price().changes, 1U);
    EXPECT_EQ(p.ask_price().to_or_from_zero, 1U);
    EXPECT_EQ(p.ask_price().down, 1U);
    EXPECT_EQ(p.ask_qty().to_or_from_zero, 1U);
}

TEST(TickProfile, AnEmptyProfileAndASingleTickCountNoDifferences) {
    TickProfile p;
    EXPECT_EQ(p.ticks(), 0U);
    EXPECT_EQ(p.time_gap().total(), 0U);
    p.add(tick(5, 0, 1, 1, 2, 1));
    EXPECT_EQ(p.ticks(), 1U);
    EXPECT_EQ(p.securities(), 1U);
    EXPECT_EQ(p.time_gap().total(), 0U) << "the first tick has no tick before it";
    EXPECT_EQ(p.same_security(), 0U) << "nor a security before it, though both locates are 0";
    EXPECT_EQ(p.ticks_since().total(), 0U);
    EXPECT_EQ(p.bid_price().changes, 0U) << "a first quote is not a change from zero";
}

// As a book's listener, over a real stream: the tables must add up.
TEST(TickProfile, AddsUpOverABooksUpdates) {
    const gen::FlowConfig cfg = test::busy_config(7);
    const std::vector<std::byte> feed_bytes = test::engine_feed(cfg, 20'000);
    using Books = book::BookManager<book::OrderStore, book::PriceLevels, TickProfile>;
    const auto books = std::make_unique<Books>(book::OrderStore{}, TickProfile{});
    feed::ItchParser parser(*books);
    ASSERT_TRUE(parser.parse(feed_bytes).ok());
    const TickProfile& p = books->listener();

    const std::vector<Tick> ticks = test::engine_ticks(cfg, 20'000);
    ASSERT_GT(ticks.size(), 5'000U);
    EXPECT_EQ(p.ticks(), ticks.size());
    EXPECT_EQ(p.time_gap().total(), p.ticks() - 1);
    EXPECT_EQ(p.time_backwards(), 0U) << "a feed's time does not go back";
    EXPECT_EQ(p.ticks_since().total(), p.ticks() - p.securities());
    const std::uint64_t compared =
        std::accumulate(p.masks().begin(), p.masks().end(), std::uint64_t{0});
    EXPECT_EQ(compared, p.ticks() - p.securities());
    EXPECT_EQ(p.masks()[0], 0U) << "a book publishes only when its best bid or offer changed";

    // Each field's changes are the ticks whose mask has its bit.
    const auto with_bit = [&p](unsigned bit) {
        std::uint64_t n = 0;
        for (unsigned mask = 0; mask < 16; ++mask) {
            n += (mask & bit) != 0 ? p.masks()[mask] : 0;
        }
        return n;
    };
    EXPECT_EQ(p.bid_price().changes, with_bit(TickProfile::kBidPrice));
    EXPECT_EQ(p.bid_qty().changes, with_bit(TickProfile::kBidQty));
    EXPECT_EQ(p.ask_price().changes, with_bit(TickProfile::kAskPrice));
    EXPECT_EQ(p.ask_qty().changes, with_bit(TickProfile::kAskQty));
    EXPECT_EQ(p.bid_qty().size.total(), p.bid_qty().changes);
    EXPECT_EQ(p.bid_qty().size.count[0], 0U) << "a change of nothing is not a change";
}

}  // namespace
