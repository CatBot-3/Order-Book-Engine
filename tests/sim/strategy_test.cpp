#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "obe/book/types.hpp"
#include "obe/sim/strategy.hpp"
#include "obe/sim/types.hpp"
#include "obe/sim/volatility.hpp"
#include "obe/types.hpp"
#include "support/sim_harness.hpp"

// The pieces a strategy is built from, and the two simple strategies.

namespace {

using namespace obe;
using test::cents;

// A market of 500 bid at `bid` and 700 offered at `ask`, in cents.
sim::MarketView market(std::uint32_t bid, std::uint32_t ask, std::int64_t inventory = 0) {
    sim::MarketView view;
    view.now = 1'000'000;
    view.bbo = {cents(bid), 500, cents(ask), 700};
    view.inventory = inventory;
    view.tick = 100;
    return view;
}

// --- The tick grid -------------------------------------------------------------

TEST(TickGrid, FloorAndCeilingLandOnTheGrid) {
    EXPECT_EQ(sim::floor_to_tick(100'050, 100), 100'000U);
    EXPECT_EQ(sim::ceil_to_tick(100'050, 100), 100'100U);
    EXPECT_EQ(sim::floor_to_tick(100'099, 100), 100'000U);
    EXPECT_EQ(sim::ceil_to_tick(100'001, 100), 100'100U);
    // A price already on the grid stays where it is, both ways.
    EXPECT_EQ(sim::floor_to_tick(100'100, 100), 100'100U);
    EXPECT_EQ(sim::ceil_to_tick(100'100, 100), 100'100U);
    EXPECT_EQ(sim::floor_to_tick(99, 100), 0U);
    EXPECT_EQ(sim::ceil_to_tick(1, 100), 100U);
    EXPECT_EQ(sim::ceil_to_tick(0, 100), 0U);
    // A tick of one price unit, and the degenerate tick of zero: no rounding.
    EXPECT_EQ(sim::floor_to_tick(123'457, 1), 123'457U);
    EXPECT_EQ(sim::ceil_to_tick(123'457, 0), 123'457U);
    EXPECT_EQ(sim::floor_to_tick(123'457, 0), 123'457U);
}

TEST(TickGrid, DollarsToPricesRoundTheWayTheyAreAsked) {
    EXPECT_EQ(sim::floor_price(99.342615), 993'426U);
    EXPECT_EQ(sim::ceil_price(99.342615), 993'427U);
    EXPECT_EQ(sim::floor_price(-0.5), 0U);
    EXPECT_EQ(sim::ceil_price(-0.5), 0U);
    EXPECT_EQ(sim::floor_price(0.0), 0U);
    EXPECT_DOUBLE_EQ(sim::mid_dollars(200'400), 10.02);
    EXPECT_DOUBLE_EQ(sim::mid_dollars(200'300), 10.015);
}

TEST(TickGrid, AResultThatShouldBeExactDoesNotLoseAUnitToFloatingPoint) {
    // 0.1 + 0.2 is not 0.3 in binary; it is a hair more. A formula whose true
    // answer is exactly 0.3000 must still floor and ceil to 3,000.
    const double a_hair_more = 0.1 + 0.2;
    ASSERT_GT(a_hair_more, 0.3);
    EXPECT_EQ(sim::ceil_price(a_hair_more), 3'000U);
    EXPECT_EQ(sim::floor_price(a_hair_more), 3'000U);
    // And a hair less: 0.7 - 0.4 is slightly under 0.3.
    const double a_hair_less = 0.7 - 0.4;
    ASSERT_LT(a_hair_less, 0.3);
    EXPECT_EQ(sim::floor_price(a_hair_less), 3'000U);
    EXPECT_EQ(sim::ceil_price(a_hair_less), 3'000U);
    // Every whole cent from one cent to $2,000 survives the trip.
    for (std::uint32_t c = 1; c <= 200'000; ++c) {
        const double dollars = static_cast<double>(c) / 100.0;
        ASSERT_EQ(sim::floor_price(dollars), cents(c)) << c;
        ASSERT_EQ(sim::ceil_price(dollars), cents(c)) << c;
    }
}

TEST(TickGrid, PassiveLimitsStopOneTickShortOfTheOtherSide) {
    const sim::MarketView view = market(1000, 1004);
    EXPECT_EQ(sim::passive_bid_limit(view), cents(1003));
    EXPECT_EQ(sim::passive_ask_limit(view), cents(1001));
    // An offer below one tick leaves no room for a bid at all.
    sim::MarketView tiny;
    tiny.bbo = {10, 100, 60, 100};
    tiny.tick = 100;
    EXPECT_EQ(sim::passive_bid_limit(tiny), 0U);
    EXPECT_EQ(sim::passive_ask_limit(tiny), 100U);
}

TEST(TickGrid, PassiveLimitsAreOnTheGridEvenWhenTheRealPricesAreNot) {
    // A stock quoted in hundredths of a cent, 0.8250 bid and 0.8275 offered,
    // seen by a strategy that quotes in whole cents.
    sim::MarketView view;
    view.bbo = {8'250, 100, 8'275, 100};
    view.tick = 100;
    // The highest whole cent below 0.8275 is 0.82; the lowest above 0.8250 is
    // 0.83. "The offer less a tick" would be 0.8175, which is not a price a
    // whole-cent quote can have.
    EXPECT_EQ(sim::passive_bid_limit(view), 8'200U);
    EXPECT_EQ(sim::passive_ask_limit(view), 8'300U);
    EXPECT_LT(sim::passive_bid_limit(view), view.bbo.ask_price);
    EXPECT_GT(sim::passive_ask_limit(view), view.bbo.bid_price);

    // An offer exactly on a tick: the limit is the tick below it, not itself.
    view.bbo = {8'250, 100, 8'300, 100};
    EXPECT_EQ(sim::passive_bid_limit(view), 8'200U);
    view.bbo = {8'200, 100, 8'275, 100};
    EXPECT_EQ(sim::passive_ask_limit(view), 8'300U);
}

TEST(MarketView, KnowsWhenThereIsAMidToQuoteAround) {
    EXPECT_TRUE(market(1000, 1004).two_sided());
    EXPECT_EQ(market(1000, 1004).mid2(), 200'400);
    EXPECT_EQ(market(1000, 1001).mid2(), 200'100) << "the mid of 10.00 and 10.01, doubled";

    sim::MarketView view = market(1000, 1004);
    view.bbo.bid_qty = 0;
    EXPECT_FALSE(view.two_sided()) << "no bid";
    view = market(1000, 1004);
    view.bbo.ask_qty = 0;
    EXPECT_FALSE(view.two_sided()) << "no offer";
    EXPECT_FALSE(market(1004, 1004).two_sided()) << "locked";
    EXPECT_FALSE(market(1005, 1004).two_sided()) << "crossed";
}

// --- NoQuotes and JoinBest -----------------------------------------------------

TEST(NoQuotes, NeverWantsAnything) {
    sim::NoQuotes strategy;
    EXPECT_EQ(strategy.quote(market(1000, 1004)), sim::Quotes{});
}

TEST(JoinBest, QuotesTheRealBestOnBothSides) {
    sim::JoinBest strategy{.qty = 200, .max_inventory = 1'000};
    const sim::Quotes q = strategy.quote(market(1000, 1004));
    EXPECT_EQ(q.bid, (sim::Quote{cents(1000), 200}));
    EXPECT_EQ(q.ask, (sim::Quote{cents(1004), 200}));
}

TEST(JoinBest, StopsAddingToAPositionAtTheLimit) {
    sim::JoinBest strategy{.qty = 100, .max_inventory = 300};
    EXPECT_TRUE(strategy.quote(market(1000, 1004, 299)).bid.live());
    sim::Quotes q = strategy.quote(market(1000, 1004, 300));
    EXPECT_FALSE(q.bid.live()) << "long the limit: no more buying";
    EXPECT_TRUE(q.ask.live());
    q = strategy.quote(market(1000, 1004, -300));
    EXPECT_TRUE(q.bid.live());
    EXPECT_FALSE(q.ask.live()) << "short the limit: no more selling";
    EXPECT_TRUE(strategy.quote(market(1000, 1004, -299)).ask.live());
}

TEST(JoinBest, WantsNothingWithoutATwoSidedMarket) {
    sim::JoinBest strategy;
    EXPECT_EQ(strategy.quote(market(1004, 1004)), sim::Quotes{});
    sim::MarketView view = market(1000, 1004);
    view.bbo.ask_qty = 0;
    EXPECT_EQ(strategy.quote(view), sim::Quotes{});
}

// --- FixedSpread ---------------------------------------------------------------

TEST(FixedSpread, QuotesTheSameDistanceEitherSideOfTheMid) {
    // Mid 10.02; three cents either side.
    sim::FixedSpread strategy{.half_spread = 300, .qty = 100, .max_inventory = 1'000};
    sim::Quotes q = strategy.quote(market(1000, 1004));
    EXPECT_EQ(q.bid, (sim::Quote{cents(999), 100}));
    EXPECT_EQ(q.ask, (sim::Quote{cents(1005), 100}));

    // One cent either side: inside the real spread.
    strategy.half_spread = 100;
    q = strategy.quote(market(1000, 1004));
    EXPECT_EQ(q.bid.price, cents(1001));
    EXPECT_EQ(q.ask.price, cents(1003));
}

TEST(FixedSpread, RoundsOutwardsWhenTheMidIsBetweenTicks) {
    // Mid 10.015. A cent either side is 10.005 and 10.025: the bid goes down
    // to 10.00 and the ask up to 10.03, never the other way.
    sim::FixedSpread strategy{.half_spread = 100, .qty = 100, .max_inventory = 1'000};
    const sim::Quotes q = strategy.quote(market(1000, 1003));
    EXPECT_EQ(q.bid.price, cents(1000));
    EXPECT_EQ(q.ask.price, cents(1003));
}

TEST(FixedSpread, StaysPassiveWhenTheRealPricesAreBetweenItsTicks) {
    // 0.8250 bid, 0.8275 offered, quoted in whole cents with no spread asked
    // for. The mid is 0.82625: the bid rounds down to 0.82 and the ask up to
    // 0.83, each on the grid and each short of the real other side.
    sim::FixedSpread strategy{.half_spread = 0, .qty = 100, .max_inventory = 1'000};
    sim::MarketView view;
    view.bbo = {8'250, 100, 8'275, 100};
    view.tick = 100;
    const sim::Quotes q = strategy.quote(view);
    EXPECT_EQ(q.bid.price, 8'200U);
    EXPECT_EQ(q.ask.price, 8'300U);
}

TEST(FixedSpread, NeverReachesTheOtherSideOfTheRealBook) {
    // A half spread of zero asks for both quotes at the mid.
    sim::FixedSpread strategy{.half_spread = 0, .qty = 100, .max_inventory = 1'000};
    // Real spread of four cents, mid on a tick: both land on 10.02, and the
    // simulator's own check refuses a bid that is not below the ask. What the
    // strategy guarantees is only that neither reaches the real other side.
    sim::Quotes q = strategy.quote(market(1000, 1004));
    EXPECT_LT(q.bid.price, cents(1004));
    EXPECT_GT(q.ask.price, cents(1000));
    // Real spread of one cent: the mid rounds onto the two real prices.
    q = strategy.quote(market(1000, 1001));
    EXPECT_EQ(q.bid.price, cents(1000));
    EXPECT_EQ(q.ask.price, cents(1001));
    // A half spread far wider than the market is left alone.
    strategy.half_spread = 5'000;
    q = strategy.quote(market(1000, 1001));
    EXPECT_EQ(q.bid.price, cents(950));
    EXPECT_EQ(q.ask.price, cents(1051));
}

TEST(FixedSpread, StopsAddingToAPositionAtTheLimit) {
    sim::FixedSpread strategy{.half_spread = 100, .qty = 100, .max_inventory = 200};
    sim::Quotes q = strategy.quote(market(1000, 1004, 200));
    EXPECT_FALSE(q.bid.live());
    EXPECT_TRUE(q.ask.live());
    q = strategy.quote(market(1000, 1004, -200));
    EXPECT_TRUE(q.bid.live());
    EXPECT_FALSE(q.ask.live());
    EXPECT_EQ(strategy.quote(market(1004, 1004)), sim::Quotes{});
}

TEST(FixedSpread, ABidThatWouldBeBelowZeroIsNotQuoted) {
    sim::FixedSpread strategy{.half_spread = 50'000, .qty = 100, .max_inventory = 1'000};
    const sim::Quotes q = strategy.quote(market(100, 104));  // a one-dollar stock
    EXPECT_FALSE(q.bid.live());
    EXPECT_TRUE(q.ask.live());
}

// --- The volatility estimate ---------------------------------------------------

constexpr Nanos kSecond = sim::kNanosPerSecond;

TEST(EwmaVariance, IsNotReadyUntilTimeHasPassedBetweenTwoMids) {
    sim::EwmaVariance variance;
    EXPECT_FALSE(variance.ready());
    EXPECT_EQ(variance.value(), 0.0);
    variance.observe(10 * kSecond, 100.0);
    EXPECT_FALSE(variance.ready());
    variance.observe(11 * kSecond, 100.0);
    EXPECT_TRUE(variance.ready());
    EXPECT_EQ(variance.value(), 0.0) << "a mid that did not move has no variance";
}

TEST(EwmaVariance, OneMoveGivesItsSquareOverTheTimeItTook) {
    sim::EwmaVariance variance;
    variance.observe(0, 100.00);
    variance.observe(4 * kSecond, 100.02);  // two cents in four seconds
    EXPECT_NEAR(variance.value(), 0.02 * 0.02 / 4.0, 1e-15);
}

TEST(EwmaVariance, WithoutFadingItIsTheSumOfSquaresOverTheTotalTime) {
    // A half-life of zero means no fading at all: every move counts alike.
    sim::EwmaVariance variance(0);
    variance.observe(0, 50.00);
    variance.observe(1 * kSecond, 50.01);
    variance.observe(3 * kSecond, 49.99);
    variance.observe(4 * kSecond, 49.99);
    const double expected = (0.01 * 0.01 + 0.02 * 0.02 + 0.0) / 4.0;
    EXPECT_NEAR(variance.value(), expected, 1e-15);
}

TEST(EwmaVariance, SeveralMovesInOneInstantAreCountedButAddNoTime) {
    sim::EwmaVariance variance(0);
    variance.observe(0, 50.00);
    variance.observe(2 * kSecond, 50.01);
    variance.observe(2 * kSecond, 50.03);  // same timestamp
    EXPECT_NEAR(variance.value(), (0.01 * 0.01 + 0.02 * 0.02) / 2.0, 1e-15);
}

TEST(EwmaVariance, DividesSumsSoThatAFastTickDoesNotExplode) {
    // A one-cent move a microsecond after the previous one. Averaging
    // move^2 / dt move by move would give 0.0001 / 0.000001 = 100 for this
    // one observation. The sums give a sensible figure.
    sim::EwmaVariance variance(0);
    variance.observe(0, 50.00);
    variance.observe(10 * kSecond, 50.01);
    variance.observe(10 * kSecond + 1'000, 50.02);
    EXPECT_NEAR(variance.value(), 2e-4 / 10.000001, 1e-12);
    EXPECT_LT(variance.value(), 1e-4);
}

TEST(EwmaVariance, OldMovesFadeWithTheHalfLife) {
    // A burst of movement, then exactly one half-life of quiet in one step.
    sim::EwmaVariance variance(60 * kSecond);
    variance.observe(0, 100.0);
    variance.observe(1 * kSecond, 100.10);
    const double before = variance.value();
    variance.observe(61 * kSecond, 100.10);
    // The old square and the old second both halve, and sixty quiet seconds
    // join the denominator: 0.5 * 0.01 over 0.5 * 1 + 60.
    EXPECT_NEAR(variance.value(), 0.5 * 0.01 / (0.5 + 60.0), 1e-12);
    EXPECT_LT(variance.value(), before);
}

TEST(EwmaVariance, FollowsAChangeInHowMuchTheMidMoves) {
    // A mid that alternates by `step` once a second has variance step^2 per
    // second. Switch from one-cent steps to five-cent steps and the estimate
    // must move from the first figure towards the second.
    sim::EwmaVariance variance(30 * kSecond);
    double mid = 100.0;
    Nanos now = 0;
    variance.observe(now, mid);
    for (int i = 0; i < 600; ++i) {
        now += kSecond;
        mid += (i % 2 == 0) ? 0.01 : -0.01;
        variance.observe(now, mid);
    }
    EXPECT_NEAR(variance.value(), 1e-4, 1e-8);
    for (int i = 0; i < 600; ++i) {
        now += kSecond;
        mid += (i % 2 == 0) ? 0.05 : -0.05;
        variance.observe(now, mid);
    }
    EXPECT_NEAR(variance.value(), 25e-4, 1e-7);
}

TEST(EwmaVariance, AnObservationOutOfOrderDoesNotMoveTimeBackwards) {
    sim::EwmaVariance variance(0);
    variance.observe(5 * kSecond, 10.00);
    variance.observe(3 * kSecond, 10.02);  // earlier than the last: treated as same instant
    variance.observe(7 * kSecond, 10.02);
    EXPECT_TRUE(std::isfinite(variance.value()));
    EXPECT_NEAR(variance.value(), 0.02 * 0.02 / 2.0, 1e-15);
}

}  // namespace
