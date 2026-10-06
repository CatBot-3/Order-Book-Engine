#include <gtest/gtest.h>

#include <cstdint>

#include "obe/book/types.hpp"
#include "obe/gen/rng.hpp"
#include "obe/sim/avellaneda_stoikov.hpp"
#include "obe/sim/simulator.hpp"
#include "obe/sim/strategy.hpp"
#include "obe/sim/types.hpp"
#include "obe/types.hpp"
#include "support/sim_harness.hpp"

// The Avellaneda-Stoikov strategy: the part of the simulator written by hand.
//
// Every test here fails until AvellanedaStoikov::quote is written. The
// expected prices were worked out from the contract in
// obe/sim/avellaneda_stoikov.hpp with a calculator, not with the code under
// test, and each test shows its working so that a failure can be checked by
// hand.
//
// The first group uses the parameters of the paper's own example (gamma 0.1,
// k 1.5), which give a spread of more than a dollar. That is unrealistic for
// a stock quoted in cents and convenient for a test: the numbers are large
// enough to follow.

namespace {

using namespace obe;
using test::cents;

constexpr Nanos kSecond = sim::kNanosPerSecond;
constexpr Nanos kNow = 1'000 * kSecond;

// The paper's example parameters, with a fixed variance so that the numbers
// do not depend on the estimator: 0.0004 dollars^2 per second, which is a
// standard deviation of two cents per second.
sim::AsParams paper() {
    sim::AsParams p;
    p.gamma = 0.1;
    p.k = 1.5;
    p.qty = 100;
    p.max_inventory = 100'000;
    p.sigma2 = 0.0004;
    return p;
}

// A real market of 99.99 bid, 100.01 offered: a mid of exactly 100 dollars,
// with `seconds_left` until the end of the session.
sim::MarketView market(std::int64_t inventory, double seconds_left = 600.0) {
    sim::MarketView view;
    view.now = kNow;
    view.bbo = {cents(9999), 500, cents(10001), 500};
    view.inventory = inventory;
    view.tick = 100;
    view.end = kNow + static_cast<Nanos>(seconds_left * 1e9);
    return view;
}

// With these parameters and 600 seconds left:
//
//   gamma * sigma^2 * tau        = 0.1 * 0.0004 * 600        = 0.024
//   (2 / gamma) * ln(1 + gamma/k) = 20 * ln(1.0666...)        = 1.290770
//   spread                        = 0.024 + 1.290770          = 1.314770
//   half the spread                                           = 0.657385

TEST(AvellanedaStoikov, WithNoInventoryTheQuotesAreCentredOnTheMid) {
    sim::AvellanedaStoikov strategy(paper());
    const sim::Quotes q = strategy.quote(market(0));
    // r = 100. Bid 100 - 0.657385 = 99.342615, down to 99.34.
    // Ask 100 + 0.657385 = 100.657385, up to 100.66.
    EXPECT_EQ(q.bid, (sim::Quote{cents(9934), 100}));
    EXPECT_EQ(q.ask, (sim::Quote{cents(10066), 100}));
}

TEST(AvellanedaStoikov, ALongPositionMovesBothQuotesDown) {
    sim::AvellanedaStoikov strategy(paper());
    const sim::Quotes q = strategy.quote(market(500));
    // q = 500 / 100 = 5. r = 100 - 5 * 0.024 = 99.88.
    // Bid 99.222615 -> 99.22. Ask 100.537385 -> 100.54.
    EXPECT_EQ(q.bid.price, cents(9922));
    EXPECT_EQ(q.ask.price, cents(10054));
}

TEST(AvellanedaStoikov, AShortPositionMovesBothQuotesUp) {
    sim::AvellanedaStoikov strategy(paper());
    const sim::Quotes q = strategy.quote(market(-500));
    // r = 100 + 5 * 0.024 = 100.12. Bid 99.462615 -> 99.46. Ask 100.777385 -> 100.78.
    EXPECT_EQ(q.bid.price, cents(9946));
    EXPECT_EQ(q.ask.price, cents(10078));
}

TEST(AvellanedaStoikov, InventoryIsCountedInQuotesNotInShares) {
    // The same 500 shares with a quote size of 250 is q = 2, not 5.
    sim::AsParams p = paper();
    p.qty = 250;
    sim::AvellanedaStoikov strategy(p);
    const sim::Quotes q = strategy.quote(market(500));
    // r = 100 - 2 * 0.024 = 99.952. Bid 99.294615 -> 99.29. Ask 100.609385 -> 100.61.
    EXPECT_EQ(q.bid, (sim::Quote{cents(9929), 250}));
    EXPECT_EQ(q.ask, (sim::Quote{cents(10061), 250}));
}

TEST(AvellanedaStoikov, AtTheEndOfTheSessionInventoryNoLongerMatters) {
    sim::AvellanedaStoikov strategy(paper());
    // tau = 0: r is the mid whatever is held, and only the second term of the
    // spread is left. Half of 1.290770 is 0.645385.
    // Bid 99.354615 -> 99.35. Ask 100.645385 -> 100.65.
    for (const std::int64_t inventory : {0, 500, -500, 5'000}) {
        const sim::Quotes q = strategy.quote(market(inventory, 0.0));
        EXPECT_EQ(q.bid.price, cents(9935)) << "inventory " << inventory;
        EXPECT_EQ(q.ask.price, cents(10065)) << "inventory " << inventory;
    }
    // A view from after the end is treated the same way, not as negative time.
    sim::MarketView late = market(500);
    late.end = kNow - 5 * kSecond;
    EXPECT_EQ(strategy.quote(late).bid.price, cents(9935));
}

TEST(AvellanedaStoikov, TheHorizonIsCappedAtMaxTau) {
    sim::AsParams p = paper();
    p.max_tau = 600.0;
    sim::AvellanedaStoikov strategy(p);
    // Ten hours left, or no end at all: the formula is given 600 seconds, and
    // the answer is the one for 600 seconds.
    sim::MarketView view = market(500, 36'000.0);
    EXPECT_EQ(strategy.quote(view).bid.price, cents(9922));
    view.end = sim::kForever;
    const sim::Quotes q = strategy.quote(view);
    EXPECT_EQ(q.bid.price, cents(9922));
    EXPECT_EQ(q.ask.price, cents(10054));
}

TEST(AvellanedaStoikov, MoreVolatilityAndMoreTimeWidenTheSpreadAndTheSkew) {
    // Four times the variance: gamma * sigma^2 * tau = 0.096.
    // spread = 0.096 + 1.290770 = 1.386770, half 0.693385.
    // With q = 5: r = 100 - 0.48 = 99.52. Bid 98.826615 -> 98.82. Ask 100.213385 -> 100.22.
    sim::AsParams p = paper();
    p.sigma2 = 0.0016;
    sim::AvellanedaStoikov noisy(p);
    sim::Quotes q = noisy.quote(market(500));
    EXPECT_EQ(q.bid.price, cents(9882));
    EXPECT_EQ(q.ask.price, cents(10022));

    // The original variance and four times the time gives the same product,
    // and so the same quotes.
    sim::AvellanedaStoikov patient(paper());
    q = patient.quote(market(500, 2'400.0));
    EXPECT_EQ(q.bid.price, cents(9882));
    EXPECT_EQ(q.ask.price, cents(10022));
}

TEST(AvellanedaStoikov, ParametersForAStockQuotedInCents) {
    // gamma 0.001, k 200, sigma^2 0.00017 (about two dollars a day on a
    // hundred-dollar stock), 10,000 seconds left, long 300 shares.
    //
    //   gamma * sigma^2 * tau         = 0.001 * 0.00017 * 10000  = 0.0017
    //   (2 / gamma) * ln(1 + gamma/k) = 2000 * ln(1.000005)      = 0.010000
    //   spread = 0.0117, half 0.00585
    //   r = 100 - 3 * 0.0017 = 99.9949
    //   bid 99.98905 -> 99.98     ask 100.00075 -> 100.01
    sim::AsParams p;
    p.gamma = 0.001;
    p.k = 200.0;
    p.qty = 100;
    p.max_inventory = 100'000;
    p.sigma2 = 0.00017;
    sim::AvellanedaStoikov strategy(p);
    const sim::Quotes q = strategy.quote(market(300, 10'000.0));
    EXPECT_EQ(q.bid.price, cents(9998));
    EXPECT_EQ(q.ask.price, cents(10001));
}

TEST(AvellanedaStoikov, QuotesStayPassiveWhenTheModelWantsToCross) {
    sim::AvellanedaStoikov strategy(paper());
    // Short 5,000 shares: q = -50, r = 100 + 50 * 0.024 = 101.2.
    // The model's bid is 100.542615 -> 100.54, above the real offer of 100.01.
    // A passive bid can go no higher than one tick below it: 100.00.
    // The ask, 101.857385 -> 101.86, is far from the market and is left alone.
    sim::Quotes q = strategy.quote(market(-5'000));
    EXPECT_EQ(q.bid.price, cents(10000));
    EXPECT_EQ(q.ask.price, cents(10186));

    // And the mirror image when long 5,000: r = 98.8, ask 99.457385 -> 99.46,
    // below the real bid of 99.99, so it is held one tick above it.
    q = strategy.quote(market(5'000));
    EXPECT_EQ(q.ask.price, cents(10000));
    EXPECT_EQ(q.bid.price, cents(9814));
}

TEST(AvellanedaStoikov, StaysPassiveWhenTheRealPricesAreBetweenItsTicks) {
    // A stock under a dollar: 0.8250 bid, 0.8275 offered, in hundredths of a
    // cent, quoted here in whole cents. Mid 0.82625.
    //   gamma 0.5, k 400, sigma^2 fixed at 0.000001, 100 seconds left
    //   gamma * sigma^2 * tau         = 0.5 * 0.000001 * 100 = 0.00005
    //   (2 / gamma) * ln(1 + gamma/k) = 4 * ln(1.00125)      = 0.004997
    //   spread 0.005047, half 0.002523
    //   flat: bid 0.823727 -> 0.82    ask 0.828773 -> 0.83
    // Both are already passive. Short 4,000 shares, q = -40:
    //   r = 0.82625 + 40 * 0.00005 = 0.82825
    //   bid 0.825727 -> 0.82          ask 0.830773 -> 0.84
    // and long 4,000: r = 0.82425, bid 0.821727 -> 0.82, ask 0.826773 -> 0.83.
    sim::AsParams p;
    p.gamma = 0.5;
    p.k = 400.0;
    p.qty = 100;
    p.max_inventory = 100'000;
    p.sigma2 = 0.000001;
    sim::AvellanedaStoikov strategy(p);
    sim::MarketView view = market(0, 100.0);
    view.bbo = {8'250, 500, 8'275, 500};
    sim::Quotes q = strategy.quote(view);
    EXPECT_EQ(q.bid.price, 8'200U);
    EXPECT_EQ(q.ask.price, 8'300U);
    view.inventory = -4'000;
    q = strategy.quote(view);
    EXPECT_EQ(q.bid.price, 8'200U);
    EXPECT_EQ(q.ask.price, 8'400U);

    // Short 40,000: r = 0.82625 + 400 * 0.00005 = 0.84625. The model's bid,
    // 0.843727 -> 0.84, is above the real offer. The highest whole cent below
    // 0.8275 is 0.82, and that is where a passive bid stops: on the grid.
    view.inventory = -40'000;
    q = strategy.quote(view);
    EXPECT_EQ(q.bid.price, 8'200U);
    EXPECT_EQ(q.bid.price % view.tick, 0U);
    // Long 40,000: r = 0.80625, ask 0.808773 -> 0.81, below the real bid. The
    // lowest whole cent above 0.8250 is 0.83.
    view.inventory = 40'000;
    q = strategy.quote(view);
    EXPECT_EQ(q.ask.price, 8'300U);
}

TEST(AvellanedaStoikov, StopsAddingToAPositionAtTheLimit) {
    sim::AsParams p = paper();
    p.max_inventory = 500;
    sim::AvellanedaStoikov strategy(p);
    sim::Quotes q = strategy.quote(market(499));
    EXPECT_TRUE(q.bid.live());
    EXPECT_TRUE(q.ask.live());
    q = strategy.quote(market(500));
    EXPECT_FALSE(q.bid.live());
    EXPECT_EQ(q.ask.price, cents(10054)) << "the other side is quoted as usual";
    q = strategy.quote(market(-500));
    EXPECT_EQ(q.bid.price, cents(9946));
    EXPECT_FALSE(q.ask.live());
}

TEST(AvellanedaStoikov, WantsNothingWithoutATwoSidedMarket) {
    sim::AvellanedaStoikov strategy(paper());
    sim::MarketView view = market(0);
    view.bbo.ask_qty = 0;
    EXPECT_EQ(strategy.quote(view), sim::Quotes{});
    view = market(0);
    view.bbo.bid_qty = 0;
    EXPECT_EQ(strategy.quote(view), sim::Quotes{});
    view = market(0);
    view.bbo.bid_price = view.bbo.ask_price;  // locked
    EXPECT_EQ(strategy.quote(view), sim::Quotes{});
}

TEST(AvellanedaStoikov, ABidThatComesOutBelowZeroIsNotQuoted) {
    sim::AvellanedaStoikov strategy(paper());
    // A fifty-cent stock: 0.49 bid, 0.51 offered. Half the spread is 0.657,
    // so the model's bid is negative.
    sim::MarketView view = market(0);
    view.bbo = {cents(49), 500, cents(51), 500};
    const sim::Quotes q = strategy.quote(view);
    EXPECT_FALSE(q.bid.live());
    // Ask 0.50 + 0.657385 = 1.157385 -> 1.16.
    EXPECT_EQ(q.ask, (sim::Quote{cents(116), 100}));
}

TEST(AvellanedaStoikov, EstimatesTheVarianceFromTheMidsItIsShown) {
    sim::AsParams p = paper();
    p.sigma2 = 0.0;       // estimate it
    p.vol_half_life = 0;  // from everything seen, without fading
    sim::AvellanedaStoikov strategy(p);

    // First view: nothing to estimate from yet, so sigma^2 is 0 and only the
    // second term of the spread is there. Mid 100, long 500.
    // Bid 100 - 0.645385 = 99.354615 -> 99.35, whatever is held.
    sim::MarketView view = market(500);
    EXPECT_EQ(strategy.quote(view).bid.price, cents(9935));

    // One second later the mid is two cents higher: 0.02^2 / 1 = 0.0004,
    // the same variance as in the tests above. Mid 100.02, 600 seconds left.
    view.now = kNow + kSecond;
    view.end = view.now + 600 * kSecond;
    view.bbo = {cents(10001), 500, cents(10003), 500};
    const sim::Quotes q = strategy.quote(view);
    EXPECT_NEAR(strategy.variance().value(), 0.0004, 1e-12);
    // r = 100.02 - 5 * 0.024 = 99.90. Bid 99.242615 -> 99.24. Ask 100.557385 -> 100.56.
    EXPECT_EQ(q.bid.price, cents(9924));
    EXPECT_EQ(q.ask.price, cents(10056));
}

TEST(AvellanedaStoikov, AFixedVarianceIsUsedInPreferenceToTheEstimate) {
    sim::AvellanedaStoikov strategy(paper());  // sigma2 fixed at 0.0004
    sim::MarketView view = market(500);
    strategy.quote(view);
    // The mid jumps a dollar in a second. The estimator sees it; the quotes
    // must not.
    view.now = kNow + kSecond;
    view.end = view.now + 600 * kSecond;
    view.bbo = {cents(10099), 500, cents(10101), 500};
    const sim::Quotes q = strategy.quote(view);
    // r = 101 - 0.12 = 100.88. Bid 100.222615 -> 100.22. Ask 101.537385 -> 101.54.
    EXPECT_EQ(q.bid.price, cents(10022));
    EXPECT_EQ(q.ask.price, cents(10154));
    EXPECT_GT(strategy.variance().value(), 0.5) << "the estimator is fed on every call";
}

TEST(AvellanedaStoikov, WhateverTheInputsTheQuotesAreUsable) {
    // Random markets, positions and times. Whatever the formula gives, the
    // result has to be something the simulator will accept: on the tick grid,
    // the bid below the ask, and neither reaching the real other side.
    gen::SplitMix64 rng(20261004);
    sim::AsParams p;
    p.gamma = 0.002;
    p.k = 150.0;
    p.qty = 100;
    p.max_inventory = 2'000;
    p.sigma2 = 0.0002;
    sim::AvellanedaStoikov strategy(p);
    for (int i = 0; i < 20'000; ++i) {
        sim::MarketView view;
        view.now = kNow;
        view.tick = 100;
        const auto bid = static_cast<std::uint32_t>(rng.between(200, 50'000));
        const auto spread = static_cast<std::uint32_t>(rng.between(1, 20));
        view.bbo = {cents(bid), 100, cents(bid + spread), 100};
        view.inventory = static_cast<std::int64_t>(rng.below(6'001)) - 3'000;
        view.end = kNow + rng.below(23'400) * kSecond;
        const sim::Quotes q = strategy.quote(view);
        if (q.bid.live()) {
            ASSERT_EQ(q.bid.price % view.tick, 0U);
            ASSERT_LT(q.bid.price, view.bbo.ask_price);
            ASSERT_EQ(q.bid.qty, 100U);
            ASSERT_LT(view.inventory, p.max_inventory);
        } else {
            ASSERT_GE(view.inventory, p.max_inventory);
        }
        if (q.ask.live()) {
            ASSERT_EQ(q.ask.price % view.tick, 0U);
            ASSERT_GT(q.ask.price, view.bbo.bid_price);
            ASSERT_EQ(q.ask.qty, 100U);
            ASSERT_GT(view.inventory, -p.max_inventory);
        } else {
            ASSERT_LE(view.inventory, -p.max_inventory);
        }
        if (q.bid.live() && q.ask.live()) {
            ASSERT_LT(q.bid.price, q.ask.price);
        }
    }
}

// --- In the simulator ------------------------------------------------------------

TEST(AvellanedaStoikov, QuotesInTheSimulatorAndLeansAgainstWhatItBuys) {
    sim::AsParams p;
    p.gamma = 0.001;
    p.k = 200.0;
    p.qty = 100;
    p.max_inventory = 1'000;
    p.sigma2 = 0.0004;
    p.max_tau = 10'000.0;
    sim::AvellanedaStoikov strategy(p);
    sim::SimConfig cfg;
    cfg.symbol = "QUOTE";
    sim::MarketMakingSim simulator(cfg, strategy);
    test::Tape tape(simulator);
    tape.list();

    // 99.99 bid, 100.01 offered.
    //   gamma * sigma^2 * tau = 0.001 * 0.0004 * 10000 = 0.004
    //   spread = 0.004 + 0.010000 = 0.014, half 0.007
    //   flat: bid 99.993 -> 99.99, ask 100.007 -> 100.01
    const OrderId real_bid = tape.add(Side::Buy, cents(9999), 300);
    tape.add(Side::Sell, cents(10001), 300);
    ASSERT_TRUE(simulator.working(Side::Buy).has_value());
    ASSERT_TRUE(simulator.working(Side::Sell).has_value());
    EXPECT_EQ(simulator.working(Side::Buy)->price, cents(9999));
    EXPECT_EQ(simulator.working(Side::Sell)->price, cents(10001));
    EXPECT_EQ(simulator.working(Side::Buy)->ahead, 300U);

    // The 300 ahead trade, then an order behind us does: we buy 100.
    const OrderId behind = tape.add(Side::Buy, cents(9999), 500);
    tape.execute(real_bid, 300);
    tape.execute(behind, 100);
    ASSERT_EQ(simulator.report().bought, 100U);

    // Long one quote: r = 100 - 0.004 = 99.996.
    //   bid 99.989 -> 99.98: a tick lower, it is less keen to buy more
    //   ask 100.003 -> 100.01: unchanged at this tick size
    ASSERT_TRUE(simulator.working(Side::Buy).has_value());
    EXPECT_EQ(simulator.working(Side::Buy)->price, cents(9998));
    EXPECT_EQ(simulator.working(Side::Sell)->price, cents(10001));
    EXPECT_TRUE(simulator.report().consistent());
}

}  // namespace
