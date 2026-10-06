#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/sim/simulator.hpp"
#include "obe/sim/types.hpp"
#include "obe/types.hpp"
#include "support/printers.hpp"
#include "support/sim_harness.hpp"

// The fill model, one rule at a time, on messages built by hand.
//
// Each test sets up a small real book, tells a scripted strategy what to
// quote, and then plays real orders around the quote. What is being pinned
// down is in the comment at the top of obe/sim/simulator.hpp: the three ways
// a quote trades, the one way it moves up the queue, and assumptions (d) to
// (f) about latency and passive quotes.

namespace {

using namespace obe;
using test::cents;
using test::kOtherLocate;
using test::kSimLocate;

using Sim = sim::MarketMakingSim<test::Scripted>;
using Resting = sim::RestingQuote;

class SimScenario : public ::testing::Test {
 protected:
    void start(sim::SimConfig cfg = {}) {
        cfg.symbol = "QUOTE";
        sim = std::make_unique<Sim>(cfg, strategy);
        tape = std::make_unique<test::Tape<Sim>>(*sim);
        tape->list();
    }

    // The market every test starts from:
    //
    //   bids   100 (b1) and then 200 (b2) at 10.00
    //   asks   150 (a1) at 10.04, and 100 (a_far) at 10.10
    //
    // so the mid is 10.02, and doubled it is 200,400 price units. Whatever
    // the test told the strategy to quote is placed once all four are there.
    void open_market() {
        const sim::Quotes wanted = strategy.want;
        strategy.want = {};
        b1 = tape->add(Side::Buy, cents(1000), 100);
        b2 = tape->add(Side::Buy, cents(1000), 200);
        a_far = tape->add(Side::Sell, cents(1010), 100);
        a1 = tape->add(Side::Sell, cents(1004), 150);
        strategy.want = wanted;
        ask_again();
    }

    // Has the strategy consulted without touching the book: a trading-state
    // message that repeats the state the security is already in.
    void ask_again() { tape->state('T'); }

    [[nodiscard]] std::optional<Resting> bid() const { return sim->working(Side::Buy); }
    [[nodiscard]] std::optional<Resting> ask() const { return sim->working(Side::Sell); }
    [[nodiscard]] sim::SimReport report() const { return sim->report(); }
    [[nodiscard]] const std::vector<sim::Fill>& fills() const { return sim->fills(); }

    static constexpr std::int64_t kMid2 = 200'400;
    // A price doubled, to set against a doubled mid.
    static constexpr std::int64_t twice(std::int64_t price) { return 2 * price; }

    test::Scripted strategy;
    std::unique_ptr<Sim> sim;
    std::unique_ptr<test::Tape<Sim>> tape;
    OrderId b1 = 0;
    OrderId b2 = 0;
    OrderId a1 = 0;
    OrderId a_far = 0;
};

// --- When the strategy is asked, and what it is shown --------------------------

TEST_F(SimScenario, TheStrategyIsNotAskedUntilTheMarketHasTwoSides) {
    start();
    tape->add(Side::Buy, cents(1000), 100);
    tape->add(Side::Buy, cents(999), 100);
    EXPECT_TRUE(strategy.seen.empty());

    tape->add(Side::Sell, cents(1004), 150);
    ASSERT_EQ(strategy.seen.size(), 1U);
    const sim::MarketView& view = strategy.seen[0];
    EXPECT_EQ(view.bbo, (book::Bbo{cents(1000), 100, cents(1004), 150}));
    EXPECT_EQ(view.now, tape->now());
    EXPECT_EQ(view.inventory, 0);
    EXPECT_EQ(view.tick, 100U);
    EXPECT_EQ(view.end, sim::kForever);
    EXPECT_FALSE(view.working_bid.live());
    EXPECT_FALSE(view.working_ask.live());
    EXPECT_EQ(view.mid2(), 200'400);
    EXPECT_EQ(report().decisions, 1U);
}

TEST_F(SimScenario, IsAskedAgainOnlyWhenTheBestBidOrOfferChanges) {
    start();
    open_market();
    const std::size_t asked = strategy.seen.size();
    tape->add(Side::Buy, cents(999), 100);    // behind the best bid
    tape->add(Side::Sell, cents(1011), 100);  // behind the best offer
    EXPECT_EQ(strategy.seen.size(), asked);
    tape->add(Side::Sell, cents(1004), 10);  // the best offer grows
    EXPECT_EQ(strategy.seen.size(), asked + 1);
    tape->add(Side::Buy, cents(1001), 10);  // a new best bid
    EXPECT_EQ(strategy.seen.size(), asked + 2);
}

TEST_F(SimScenario, OnlyTheQuotedSecurityMatters) {
    start();
    strategy.bid(cents(1000));
    tape->add(Side::Buy, cents(1000), 100, kOtherLocate);
    const OrderId other = tape->add(Side::Sell, cents(1004), 100, kOtherLocate);
    tape->execute(other, 50, kOtherLocate);
    EXPECT_TRUE(strategy.seen.empty());
    EXPECT_FALSE(bid().has_value());
    // The other security's book is still kept: the simulator is a full replay.
    EXPECT_EQ(sim->book().book(kOtherLocate).bbo(), (book::Bbo{cents(1000), 100, cents(1004), 50}));
    EXPECT_EQ(sim->locate(), std::optional<Locate>(kSimLocate));
}

TEST_F(SimScenario, ASecurityThatIsNeverListedIsNeverQuoted) {
    sim::SimConfig cfg;
    cfg.symbol = "ABSENT";
    sim = std::make_unique<Sim>(cfg, strategy);
    tape = std::make_unique<test::Tape<Sim>>(*sim);
    tape->list();
    strategy.bid(cents(1000));
    open_market();
    EXPECT_TRUE(strategy.seen.empty());
    EXPECT_FALSE(sim->locate().has_value());
    EXPECT_EQ(report().decisions, 0U);
    EXPECT_EQ(report().placed, 0U);
    EXPECT_EQ(report().market_executed, 0U);
}

// --- Joining the queue ---------------------------------------------------------

TEST_F(SimScenario, AQuoteJoinsBehindEverythingDisplayedAtItsPrice) {
    start();
    strategy.bid(cents(1000));
    strategy.ask(cents(1004));
    open_market();
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 300}));
    EXPECT_EQ(ask(), (Resting{cents(1004), 100, 150}));
    EXPECT_EQ(report().placed, 2U);

    // And the strategy is shown both, each on its own side.
    ask_again();
    EXPECT_EQ(strategy.seen.back().working_bid, (sim::Quote{cents(1000), 100}));
    EXPECT_EQ(strategy.seen.back().working_ask, (sim::Quote{cents(1004), 100}));
}

TEST_F(SimScenario, AQuoteAtAPriceNobodyIsAtHasNothingAhead) {
    start();
    strategy.bid(cents(1001));  // inside the spread
    strategy.ask(cents(1007));  // between two real levels
    open_market();
    EXPECT_EQ(bid(), (Resting{cents(1001), 100, 0}));
    EXPECT_EQ(ask(), (Resting{cents(1007), 100, 0}));
}

TEST_F(SimScenario, AQuoteBehindTheBestJoinsItsOwnLevel) {
    start();
    tape->add(Side::Buy, cents(999), 70);
    strategy.bid(cents(999));
    strategy.ask(cents(1010));
    open_market();
    EXPECT_EQ(bid(), (Resting{cents(999), 100, 70}));
    EXPECT_EQ(ask(), (Resting{cents(1010), 100, 100}));
}

TEST_F(SimScenario, AQuoteKeepsItsPlaceWhileItsPriceIsUnchanged) {
    start();
    strategy.bid(cents(1000));
    open_market();
    ask_again();
    tape->add(Side::Sell, cents(1004), 10);  // the best offer changes
    tape->add(Side::Buy, cents(1000), 10);   // so does the best bid; this one is behind us
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 300}));
    EXPECT_EQ(report().placed, 1U);
    EXPECT_EQ(report().cancelled, 0U);
    // The strategy is told what it has resting.
    EXPECT_EQ(strategy.seen.back().working_bid, (sim::Quote{cents(1000), 100}));
    EXPECT_FALSE(strategy.seen.back().working_ask.live());
}

TEST_F(SimScenario, AChangeOfPriceGoesToTheBackOfTheNewLevel) {
    start();
    tape->add(Side::Buy, cents(999), 70);
    strategy.bid(cents(1000));
    open_market();
    strategy.bid(cents(999));
    ask_again();
    EXPECT_EQ(bid(), (Resting{cents(999), 100, 70}));
    EXPECT_EQ(report().cancelled, 1U);
    EXPECT_EQ(report().placed, 2U);

    // And back again: it does not get its old place back.
    strategy.bid(cents(1000));
    ask_again();
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 300}));
}

TEST_F(SimScenario, NoLongerWantingAQuoteCancelsIt) {
    start();
    strategy.bid(cents(1000));
    strategy.ask(cents(1004));
    open_market();
    strategy.no_bid();
    ask_again();
    EXPECT_FALSE(bid().has_value());
    EXPECT_TRUE(ask().has_value());
    EXPECT_EQ(report().cancelled, 1U);
}

// --- Moving up the queue -------------------------------------------------------

TEST_F(SimScenario, WhatLeavesFromAheadShortensTheQueue) {
    start();
    tape->add(Side::Buy, cents(999), 70);  // keeps a real bid in the book throughout
    strategy.bid(cents(1000));
    open_market();
    tape->execute(b1, 60);
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 240}));
    tape->execute(b1, 40);  // b1 is gone
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 200}));
    tape->cancel(b2, 50);
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 150}));
    tape->remove(b2);
    // At the front, and still whole: getting there is not a trade.
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 0}));
    EXPECT_TRUE(fills().empty());
}

TEST_F(SimScenario, QuotesAreWithdrawnWhenTheRealMarketLosesASide) {
    start();
    strategy.bid(cents(1000));
    strategy.ask(cents(1004));
    open_market();
    const std::uint64_t asked = report().decisions;
    tape->remove(b1);
    tape->remove(b2);  // no real bid left: there is no mid to quote around
    EXPECT_FALSE(bid().has_value());
    EXPECT_FALSE(ask().has_value());
    EXPECT_EQ(report().cancelled, 2U);
    EXPECT_EQ(report().decisions, asked + 1) << "asked after b1 went, not after b2";

    tape->add(Side::Buy, cents(1000), 50);
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 50}));
    EXPECT_TRUE(ask().has_value());
}

TEST_F(SimScenario, WhatArrivesLaterIsBehindAndItsLeavingChangesNothing) {
    start();
    strategy.bid(cents(1000));
    open_market();
    const OrderId later = tape->add(Side::Buy, cents(1000), 500);
    tape->cancel(later, 200);
    EXPECT_EQ(bid()->ahead, 300U);
    tape->remove(later);
    EXPECT_EQ(bid()->ahead, 300U);
    EXPECT_TRUE(fills().empty());
}

TEST_F(SimScenario, AReplacedOrderLeavesTheQueueAndComesBackBehindUs) {
    start();
    strategy.bid(cents(1000));
    open_market();
    // Same price, new reference number: the exchange puts it at the back.
    const OrderId again = tape->replace(b1, cents(1000), 100);
    EXPECT_EQ(bid()->ahead, 200U);
    tape->cancel(again, 50);
    EXPECT_EQ(bid()->ahead, 200U);
}

TEST_F(SimScenario, ChangesAtOtherPricesDoNotMoveTheQueue) {
    start();
    const OrderId deeper = tape->add(Side::Buy, cents(999), 70);
    strategy.bid(cents(1000));
    open_market();
    tape->cancel(deeper, 30);
    tape->remove(deeper);
    tape->execute(a1, 50);
    tape->remove(a_far);
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 300}));
    EXPECT_TRUE(fills().empty());
}

// --- The three ways a quote trades ---------------------------------------------

TEST_F(SimScenario, AnExecutionBehindUsMeansWeTradedFirst) {
    start();
    strategy.bid(cents(1000));
    open_market();
    const OrderId behind = tape->add(Side::Buy, cents(1000), 500);
    tape->execute(b1, 100);
    tape->execute(b2, 200);
    ASSERT_EQ(bid(), (Resting{cents(1000), 100, 0}));
    ASSERT_TRUE(fills().empty());

    tape->execute(behind, 60);
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0], (sim::Fill{tape->now(), Side::Buy, cents(1000), 60, kMid2,
                                     sim::FillReason::QueueReached}));
    EXPECT_EQ(bid(), (Resting{cents(1000), 40, 0}));

    // No more than the quote has left, however much trades.
    strategy.no_bid();
    tape->execute(behind, 100);
    ASSERT_EQ(fills().size(), 2U);
    EXPECT_EQ(fills()[1].qty, 40U);
    EXPECT_FALSE(bid().has_value());
    EXPECT_EQ(report().bought, 100U);
    EXPECT_EQ(report().fills_queue, 2U);
    EXPECT_EQ(report().inventory, 100);
}

TEST_F(SimScenario, AFilledQuoteThatIsStillWantedGoesToTheBack) {
    start();
    strategy.bid(cents(1000));
    open_market();
    const OrderId behind = tape->add(Side::Buy, cents(1000), 500);
    tape->remove(b1);
    tape->remove(b2);
    tape->execute(behind, 100);  // fills all 100 of ours
    ASSERT_EQ(report().bought, 100U);
    // A new quote, behind the 400 that are left of the order that was behind.
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 400}));
    EXPECT_EQ(report().placed, 2U);
    EXPECT_EQ(strategy.seen.back().inventory, 100);
}

TEST_F(SimScenario, ATradeAtAWorsePriceMeansItPassedOurs) {
    start();
    strategy.bid(cents(1001));  // better than every real bid
    open_market();
    tape->execute(b1, 30);  // a seller reached the real bid at 10.00
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0], (sim::Fill{tape->now(), Side::Buy, cents(1001), 30, kMid2,
                                     sim::FillReason::TradedThrough}));
    EXPECT_EQ(bid(), (Resting{cents(1001), 70, 0}));
    EXPECT_EQ(report().fills_through, 1U);
}

TEST_F(SimScenario, ATradeAtABetterPriceThanOursIsNotOurs) {
    start();
    tape->add(Side::Buy, cents(999), 70);
    strategy.bid(cents(999));
    open_market();
    tape->execute(b1, 100);
    tape->execute(b2, 200);
    EXPECT_TRUE(fills().empty());
    EXPECT_EQ(bid(), (Resting{cents(999), 100, 70}));
}

TEST_F(SimScenario, ExecutionsOnTheOtherSideAreNotOurs) {
    start();
    strategy.bid(cents(1000));
    open_market();
    tape->execute(a1, 150);
    tape->execute(a_far, 50);
    EXPECT_TRUE(fills().empty());
    EXPECT_EQ(bid()->ahead, 300U);
}

TEST_F(SimScenario, AnOrderArrivingAtOurPriceWouldHaveTradedWithUs) {
    start();
    strategy.bid(cents(1001));
    open_market();
    tape->add(Side::Sell, cents(1003), 50);  // does not reach 10.01
    ASSERT_TRUE(fills().empty());

    tape->add(Side::Sell, cents(1001), 30);
    ASSERT_EQ(fills().size(), 1U);
    // The mid is the real one from before the arrival: 10.00 and 10.03.
    EXPECT_EQ(fills()[0], (sim::Fill{tape->now(), Side::Buy, cents(1001), 30, 200'300,
                                     sim::FillReason::CrossedByAdd}));

    tape->add(Side::Sell, cents(1000), 500);  // more than we have left
    ASSERT_EQ(fills().size(), 2U);
    EXPECT_EQ(fills()[1].qty, 70U);
    EXPECT_EQ(report().fills_crossed, 2U);
}

TEST_F(SimScenario, ButNotWhenARealOrderIsAtLeastAsGoodAsOurs) {
    start();
    strategy.bid(cents(1000));  // level with the real best bid, and behind it
    open_market();
    // A sell resting at 10.00 did not trade with the real bids at 10.00, so
    // the recorded book is locked. Nothing can be concluded about ours.
    tape->add(Side::Sell, cents(1000), 50);
    EXPECT_TRUE(fills().empty());
}

TEST_F(SimScenario, AReplaceThatReachesOurQuoteIsAnArrivalToo) {
    start();
    strategy.bid(cents(1001));
    open_market();
    tape->replace(a1, cents(1001), 40);
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0].qty, 40U);
    EXPECT_EQ(fills()[0].reason, sim::FillReason::CrossedByAdd);
}

TEST_F(SimScenario, AnExecutionWithItsOwnPriceIsJudgedByWhereTheOrderWasDisplayed) {
    start();
    strategy.bid(cents(1000));
    open_market();
    const OrderId behind = tape->add(Side::Buy, cents(1000), 500);
    tape->remove(b1);
    tape->remove(b2);
    tape->execute_at(behind, 50, cents(995));
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0].price, cents(1000)) << "a quote trades at its own price";
    EXPECT_EQ(fills()[0].qty, 50U);
    EXPECT_EQ(fills()[0].reason, sim::FillReason::QueueReached);

    // An execution marked non-printable is still an execution: the shares
    // traded, in a cross that reports its volume separately. Only the count of
    // the market's volume leaves it out.
    tape->execute_at(behind, 20, cents(995), false);
    ASSERT_EQ(fills().size(), 2U);
    EXPECT_EQ(fills()[1].qty, 20U);
    EXPECT_EQ(report().market_executed, 50U);
}

TEST_F(SimScenario, AFillThatLeavesTheBestPricesAloneStillHasTheStrategyAsked) {
    start();
    const OrderId deeper = tape->add(Side::Buy, cents(999), 70);
    strategy.bid(cents(999));
    open_market();
    ASSERT_EQ(bid(), (Resting{cents(999), 100, 70}));
    const OrderId behind = tape->add(Side::Buy, cents(999), 500);
    tape->remove(deeper);
    ASSERT_EQ(bid(), (Resting{cents(999), 100, 0}));
    const std::size_t asked = strategy.seen.size();

    // A trade one level below the best bid: the best bid and offer do not
    // change, so the fill is the only reason to ask the strategy again.
    tape->execute(behind, 100);
    ASSERT_EQ(report().bought, 100U);
    ASSERT_EQ(strategy.seen.size(), asked + 1);
    EXPECT_EQ(strategy.seen.back().inventory, 100);
    EXPECT_FALSE(strategy.seen.back().working_bid.live()) << "the filled quote has gone";
    // It still wants the bid, so a new one joins behind what is left there.
    EXPECT_EQ(bid(), (Resting{cents(999), 100, 400}));
}

TEST_F(SimScenario, TradesAgainstHiddenOrdersAreIgnored) {
    start();
    strategy.bid(cents(1001));
    open_market();
    const std::size_t asked = strategy.seen.size();
    tape->hidden_trade(Side::Buy, cents(1000), 10'000);
    tape->hidden_trade(Side::Sell, cents(1001), 10'000);
    EXPECT_TRUE(fills().empty());
    EXPECT_EQ(strategy.seen.size(), asked);
    EXPECT_EQ(bid(), (Resting{cents(1001), 100, 0}));
}

TEST_F(SimScenario, TheMarketsOwnVolumeCountsPrintableExecutionsOnly) {
    start();
    open_market();
    tape->execute(b1, 10);
    tape->execute_at(b1, 20, cents(1000), false);  // will be reported again by a cross
    tape->execute_at(b1, 5, cents(1000), true);
    tape->cancel(b2, 50);
    EXPECT_EQ(report().market_executed, 15U);
}

// --- The other side, mirrored --------------------------------------------------

TEST_F(SimScenario, AnAskMovesUpItsQueueAndTradesTheSameThreeWays) {
    start();
    strategy.ask(cents(1004));
    open_market();
    const OrderId behind = tape->add(Side::Sell, cents(1004), 500);
    ASSERT_EQ(ask(), (Resting{cents(1004), 100, 150}));
    tape->cancel(a1, 50);
    EXPECT_EQ(ask()->ahead, 100U);
    tape->execute(a1, 100);
    EXPECT_EQ(ask()->ahead, 0U);
    ASSERT_TRUE(fills().empty());

    tape->execute(behind, 30);  // way 1
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0], (sim::Fill{tape->now(), Side::Sell, cents(1004), 30, kMid2,
                                     sim::FillReason::QueueReached}));
    EXPECT_EQ(report().sold, 30U);
    EXPECT_EQ(report().inventory, -30);
}

TEST_F(SimScenario, AnAskInsideTheSpreadIsTradedThroughAndReached) {
    start();
    strategy.ask(cents(1003));
    open_market();
    tape->execute(a1, 20);  // way 2: a buyer paid 10.04 with 10.03 on offer
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0].reason, sim::FillReason::TradedThrough);
    EXPECT_EQ(fills()[0].price, cents(1003));

    tape->add(Side::Buy, cents(1002), 10);  // does not reach 10.03
    ASSERT_EQ(fills().size(), 1U);
    tape->add(Side::Buy, cents(1003), 25);  // way 3
    ASSERT_EQ(fills().size(), 2U);
    EXPECT_EQ(fills()[1].qty, 25U);
    EXPECT_EQ(fills()[1].reason, sim::FillReason::CrossedByAdd);
    EXPECT_EQ(ask(), (Resting{cents(1003), 55, 0}));
}

TEST_F(SimScenario, ATradeAtABetterAskThanOursIsNotOurs) {
    start();
    strategy.ask(cents(1010));
    open_market();
    tape->execute(a1, 150);
    EXPECT_TRUE(fills().empty());
    EXPECT_EQ(ask(), (Resting{cents(1010), 100, 100}));
}

// --- Messages the book refuses -------------------------------------------------

TEST_F(SimScenario, WhatTheBookRefusesIsRefusedHereToo) {
    start();
    strategy.bid(cents(1000));
    open_market();
    // A second order under b1's number is not an order. If it were taken for
    // a new arrival, b1 would count as behind us from here on.
    tape->add_as(b1, Side::Buy, cents(1000), 999);
    tape->add_as(9'999, Side::Buy, cents(1000), 0);  // no shares
    tape->execute(123'456, 50);                      // nobody
    tape->remove(123'457);
    tape->replace(123'458, cents(1000), 50);
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 300}));
    EXPECT_TRUE(fills().empty());

    tape->execute(b1, 100);  // b1 is still ahead of us
    EXPECT_EQ(bid()->ahead, 200U);
    EXPECT_TRUE(fills().empty());
    EXPECT_EQ(sim->book().counters().duplicate_order, 1U);
    EXPECT_EQ(sim->book().counters().zero_shares, 1U);
    EXPECT_EQ(sim->book().counters().unknown_order, 3U);
}

TEST_F(SimScenario, AnExecutionForMoreThanAnOrderHasTakesOnlyWhatIsThere) {
    start();
    strategy.bid(cents(1000));
    open_market();
    tape->execute(b1, 5'000);  // b1 has 100
    EXPECT_EQ(bid()->ahead, 200U);
    EXPECT_EQ(report().market_executed, 100U);
    EXPECT_EQ(sim->book().counters().overfill, 1U);
}

// --- Quotes that are refused ---------------------------------------------------

TEST_F(SimScenario, AQuoteThatWouldTradeAtOnceIsRefused) {
    start();
    strategy.bid(cents(1004));  // the real best offer
    strategy.ask(cents(1000));  // the real best bid
    open_market();
    EXPECT_FALSE(bid().has_value());
    EXPECT_FALSE(ask().has_value());
    EXPECT_EQ(report().rejected_self_cross, 1U) << "and a bid above the ask is refused first";

    strategy.no_ask();
    ask_again();
    EXPECT_FALSE(bid().has_value());
    EXPECT_EQ(report().rejected_crossing, 1U);

    strategy.no_bid();
    strategy.ask(cents(1000));  // exactly the real best bid: it would trade
    ask_again();
    EXPECT_FALSE(ask().has_value());
    EXPECT_EQ(report().rejected_crossing, 2U);
    strategy.ask(cents(999));  // and below it
    ask_again();
    EXPECT_FALSE(ask().has_value());
    EXPECT_EQ(report().rejected_crossing, 3U);
    EXPECT_EQ(report().placed, 0U);

    // One tick short of the other side is passive, and is accepted.
    strategy.bid(cents(1003));
    strategy.ask(cents(1004));
    ask_again();
    EXPECT_EQ(bid(), (Resting{cents(1003), 100, 0}));
    strategy.bid(cents(999));
    strategy.ask(cents(1001));
    ask_again();
    EXPECT_EQ(ask(), (Resting{cents(1001), 100, 0}));
}

TEST_F(SimScenario, AQuoteOffTheTickGridIsRefused) {
    start();
    strategy.bid(cents(1000) + 50);  // 10.005
    strategy.ask(cents(1004));
    open_market();
    EXPECT_FALSE(bid().has_value());
    EXPECT_TRUE(ask().has_value());
    EXPECT_EQ(report().rejected_off_tick, 1U);
}

TEST_F(SimScenario, ABidThatIsNotBelowTheAskIsRefusedWithIt) {
    start();
    strategy.bid(cents(1002));
    strategy.ask(cents(1002));
    open_market();
    EXPECT_FALSE(bid().has_value());
    EXPECT_FALSE(ask().has_value());
    EXPECT_EQ(report().rejected_self_cross, 1U);
    EXPECT_EQ(report().placed, 0U);
}

TEST_F(SimScenario, BothQuotesCanMoveUpTogether) {
    start();
    strategy.bid(cents(1001));
    strategy.ask(cents(1002));
    open_market();
    ASSERT_EQ(ask(), (Resting{cents(1002), 100, 0}));

    // The new bid goes where the old ask is. The old ask's cancel counts as
    // arriving first, so the two never meet.
    strategy.bid(cents(1002));
    strategy.ask(cents(1003));
    ask_again();
    EXPECT_EQ(bid(), (Resting{cents(1002), 100, 0}));
    EXPECT_EQ(ask(), (Resting{cents(1003), 100, 0}));
    EXPECT_EQ(report().rejected_self_cross, 0U);
    EXPECT_EQ(report().cancelled, 2U);
    EXPECT_EQ(report().placed, 4U);
}

// --- Halts and the quoting window ----------------------------------------------

TEST_F(SimScenario, AHaltPullsTheQuotesAndAResumptionRestoresThem) {
    start();
    strategy.bid(cents(1000));
    strategy.ask(cents(1004));
    open_market();
    ASSERT_EQ(report().placed, 2U);

    tape->state('H');
    EXPECT_FALSE(bid().has_value());
    EXPECT_FALSE(ask().has_value());
    EXPECT_EQ(report().cancelled, 2U);
    const std::uint64_t asked = report().decisions;
    tape->add(Side::Sell, cents(1004), 10);  // the best offer changes during the halt
    EXPECT_EQ(report().decisions, asked) << "nobody is asked for quotes during a halt";
    EXPECT_FALSE(bid().has_value());

    tape->state('T');
    EXPECT_TRUE(bid().has_value());
    EXPECT_TRUE(ask().has_value());
    EXPECT_EQ(report().placed, 4U);
}

TEST_F(SimScenario, OnlyTheTradingStateIsOpenForQuotes) {
    start();
    strategy.bid(cents(1000));
    strategy.ask(cents(1004));
    open_market();
    std::uint64_t cancelled = 0;
    // Paused, quotation only, and a state byte nobody defined: none of them
    // is continuous trading.
    for (const char state : {'P', 'Q', 'X'}) {
        ASSERT_TRUE(bid().has_value()) << state;
        tape->state(state);
        EXPECT_FALSE(bid().has_value()) << state;
        EXPECT_FALSE(ask().has_value()) << state;
        cancelled += 2;
        EXPECT_EQ(report().cancelled, cancelled) << state;
        tape->state('T');
    }
    EXPECT_TRUE(ask().has_value());
}

TEST_F(SimScenario, ASecurityNobodyHasOpenedIsNotQuoted) {
    // Listed, with a two-sided book, but no trading-state message yet.
    sim::SimConfig cfg;
    cfg.symbol = "QUOTE";
    sim = std::make_unique<Sim>(cfg, strategy);
    sim->on_stock_directory(engine::md::directory(kSimLocate, test::kSimSymbol, 500));
    tape = std::make_unique<test::Tape<Sim>>(*sim);
    strategy.bid(cents(1000));
    tape->add(Side::Buy, cents(1000), 100);
    tape->add(Side::Sell, cents(1004), 100);
    EXPECT_TRUE(strategy.seen.empty());
    EXPECT_FALSE(bid().has_value());

    tape->state('T');
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 100}));
}

TEST_F(SimScenario, NothingRestsBeforeTheWindowOpens) {
    sim::SimConfig cfg;
    cfg.start = 1'000'000;
    start(cfg);
    strategy.bid(cents(1000));
    open_market();
    ask_again();
    EXPECT_TRUE(strategy.seen.empty());
    EXPECT_FALSE(bid().has_value());

    tape->at(1'000'000);
    ask_again();
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 300}));
}

TEST_F(SimScenario, AtTheEndOfTheWindowTheQuotesAreAlreadyOut) {
    sim::SimConfig cfg;
    cfg.end = 2'000'000;
    cfg.latency = 50'000;
    start(cfg);
    strategy.bid(cents(1001));
    open_market();
    tape->at(tape->now() + 50'000);
    ask_again();
    ASSERT_TRUE(bid().has_value());
    EXPECT_EQ(strategy.seen.back().end, 2'000'000U);

    // The end is known in advance, so no latency applies to leaving: the
    // first message at or after it finds nothing of ours to trade with. The
    // window is half open: the end itself is outside it.
    const std::uint64_t asked = report().decisions;
    tape->at(2'000'000);
    tape->execute(b1, 30);
    EXPECT_TRUE(fills().empty());
    EXPECT_FALSE(bid().has_value());
    EXPECT_EQ(report().cancelled, 1U);
    EXPECT_EQ(report().decisions, asked) << "nobody is asked for quotes at the end itself";
    EXPECT_FALSE(sim->in_flight(Side::Buy));
    ask_again();
    EXPECT_EQ(report().decisions, asked);
    EXPECT_EQ(report().market_executed, 0U) << "nor does a trade at the end count as inside";
}

TEST_F(SimScenario, TheLastInstantBeforeTheEndIsStillInside) {
    sim::SimConfig cfg;
    cfg.end = 2'000'000;
    start(cfg);
    strategy.bid(cents(1001));
    open_market();
    tape->at(1'999'999);
    tape->execute(b1, 30);
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(report().market_executed, 30U);
}

TEST_F(SimScenario, AQuoteStillOnItsWayWhenTheWindowClosesNeverRests) {
    sim::SimConfig cfg;
    cfg.end = 2'000'000;
    cfg.latency = 50'000;
    start(cfg);
    open_market();
    strategy.bid(cents(1000));
    tape->at(1'990'000);
    ask_again();  // decided 10,000 before the end, due 40,000 after it
    ASSERT_TRUE(sim->in_flight(Side::Buy));
    tape->at(2'040'000);
    ask_again();
    EXPECT_FALSE(bid().has_value());
    EXPECT_FALSE(sim->in_flight(Side::Buy));
    EXPECT_EQ(report().rejected_closed, 1U);
    EXPECT_EQ(report().placed, 0U);
}

// --- Latency -------------------------------------------------------------------

class SimLatency : public SimScenario {
 protected:
    void SetUp() override {
        sim::SimConfig cfg;
        cfg.latency = 500;
        start(cfg);
    }
};

TEST_F(SimLatency, ANewQuoteJoinsTheQueueOnlyWhenItArrives) {
    strategy.bid(cents(1000));
    open_market();
    const Nanos decided = tape->now();
    EXPECT_FALSE(bid().has_value());
    EXPECT_TRUE(sim->in_flight(Side::Buy));

    // An order that arrives while ours is on its way is ahead of ours.
    tape->at(decided + 499);
    tape->add(Side::Buy, cents(1000), 50);
    EXPECT_FALSE(bid().has_value());

    tape->at(decided + 500);
    ask_again();
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 350}));
    EXPECT_FALSE(sim->in_flight(Side::Buy));
    EXPECT_EQ(report().placed, 1U);
}

TEST_F(SimLatency, TheQueueIsMeasuredBeforeTheMessageThatBringsTheTime) {
    strategy.bid(cents(1000));
    open_market();
    const Nanos decided = tape->now();
    // The quote was due at +500. The first message after that is an order at
    // its price at +800: ours was in the queue before it, so it is behind us.
    tape->at(decided + 800);
    const OrderId later = tape->add(Side::Buy, cents(1000), 50);
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 300}));
    tape->remove(later);
    EXPECT_EQ(bid()->ahead, 300U);
}

TEST_F(SimLatency, ACancelledQuoteCanStillBeHitWhileTheCancelIsOnItsWay) {
    strategy.bid(cents(1001));
    open_market();
    tape->at(tape->now() + 500);
    ask_again();
    ASSERT_TRUE(bid().has_value());

    strategy.no_bid();
    ask_again();
    const Nanos decided = tape->now();
    ASSERT_TRUE(sim->in_flight(Side::Buy));
    ASSERT_TRUE(bid().has_value()) << "still resting until the cancel arrives";

    tape->at(decided + 100);
    tape->execute(b1, 30);
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0].qty, 30U);

    tape->at(decided + 500);
    tape->execute(b1, 30);
    EXPECT_EQ(fills().size(), 1U) << "the cancel arrived before this trade";
    EXPECT_FALSE(bid().has_value());
    EXPECT_EQ(report().cancelled, 1U);
}

TEST_F(SimLatency, OneChangeIsInFlightAtATime) {
    tape->add(Side::Buy, cents(999), 70);
    strategy.bid(cents(1000));
    open_market();
    const Nanos decided = tape->now();

    // A second thought while the first is on its way has to wait.
    strategy.bid(cents(999));
    tape->at(decided + 100);
    ask_again();
    EXPECT_FALSE(bid().has_value());

    // The first lands, the strategy is asked again, and the second sets off.
    // The message that brings the time is an order far from the best prices:
    // nothing about it would have the strategy asked, except that a change of
    // its own has just taken effect.
    const std::size_t asked = strategy.seen.size();
    tape->at(decided + 500);
    tape->add(Side::Buy, cents(990), 10);
    EXPECT_EQ(bid(), (Resting{cents(1000), 100, 300}));
    EXPECT_EQ(strategy.seen.size(), asked + 1);
    EXPECT_EQ(strategy.seen.back().working_bid, (sim::Quote{cents(1000), 100}));
    EXPECT_TRUE(sim->in_flight(Side::Buy));

    tape->at(decided + 1'000);
    tape->add(Side::Buy, cents(990), 10);
    EXPECT_EQ(bid(), (Resting{cents(999), 100, 70}));
    EXPECT_EQ(report().placed, 2U);
    EXPECT_EQ(report().cancelled, 1U);
    EXPECT_EQ(strategy.seen.size(), asked + 2);
}

TEST_F(SimLatency, BothQuotesCanMoveUpTogetherWhenTheirChangesArriveTogether) {
    strategy.bid(cents(1001));
    strategy.ask(cents(1002));
    open_market();
    const Nanos decided = tape->now();
    tape->at(decided + 500);
    ask_again();
    ASSERT_EQ(ask(), (Resting{cents(1002), 100, 0}));

    strategy.bid(cents(1002));
    strategy.ask(cents(1003));
    tape->at(decided + 600);
    ask_again();
    tape->at(decided + 1'100);
    ask_again();
    EXPECT_EQ(bid(), (Resting{cents(1002), 100, 0}));
    EXPECT_EQ(ask(), (Resting{cents(1003), 100, 0}));
    EXPECT_EQ(report().rejected_self_cross, 0U);
}

TEST_F(SimLatency, ANewQuoteMayNotReachOurOwnOnTheOtherSide) {
    strategy.ask(cents(1002));
    open_market();
    const Nanos decided = tape->now();

    // While the ask is on its way the strategy changes its mind: a bid at
    // 10.02 and the ask a cent higher. The ask's change has to wait for the
    // one in flight; the bid sets off now.
    strategy.bid(cents(1002));
    strategy.ask(cents(1003));
    tape->at(decided + 100);
    ask_again();

    tape->at(decided + 500);
    ask_again();  // the ask lands at 10.02, and its move to 10.03 sets off
    ASSERT_EQ(ask(), (Resting{cents(1002), 100, 0}));

    tape->at(decided + 600);
    ask_again();  // the bid arrives at 10.02 while our ask is still there
    EXPECT_FALSE(bid().has_value());
    EXPECT_EQ(report().rejected_self_cross, 1U);
}

TEST_F(SimLatency, ChangesLandInTheOrderOfTheirTimesHoweverLateTheNextMessageIs) {
    strategy.ask(cents(1002));
    open_market();
    const Nanos decided = tape->now();
    strategy.bid(cents(1002));
    strategy.ask(cents(1003));
    tape->at(decided + 100);
    ask_again();
    tape->at(decided + 500);
    ask_again();
    // As in the test above, but nothing happens in this security until long
    // after both changes were due. The bid (due at +600) still arrived before
    // the ask's cancel (due at +1,000), and is still refused.
    tape->at(decided + 50'000);
    ask_again();
    EXPECT_FALSE(bid().has_value());
    EXPECT_EQ(report().rejected_self_cross, 1U);
    EXPECT_EQ(ask(), (Resting{cents(1003), 100, 0}));
}

TEST_F(SimLatency, AQuoteThatWouldTradeByTheTimeItArrivesIsRefused) {
    strategy.bid(cents(1003));
    open_market();
    const Nanos decided = tape->now();
    tape->at(decided + 100);
    tape->add(Side::Sell, cents(1003), 10);  // the market moves onto our price
    tape->at(decided + 500);
    ask_again();
    EXPECT_FALSE(bid().has_value());
    EXPECT_EQ(report().rejected_crossing, 1U);
}

// --- Money ---------------------------------------------------------------------

TEST_F(SimScenario, BuyingAtTheBidAndSellingAtTheOfferEarnsTheSpread) {
    start();
    strategy.bid(cents(1000));
    strategy.ask(cents(1004));
    open_market();
    const OrderId bid_behind = tape->add(Side::Buy, cents(1000), 500);
    const OrderId ask_behind = tape->add(Side::Sell, cents(1004), 500);
    tape->remove(b1);
    tape->remove(b2);
    tape->remove(a1);
    tape->execute(bid_behind, 100);
    tape->execute(ask_behind, 100);

    const sim::SimReport r = report();
    EXPECT_EQ(r.fills, 2U);
    EXPECT_EQ(r.bought, 100U);
    EXPECT_EQ(r.sold, 100U);
    EXPECT_EQ(r.inventory, 0);
    // Bought 100 at 10.00 and sold 100 at 10.04: four cents a share.
    EXPECT_EQ(r.cash, 100 * 400);
    EXPECT_EQ(r.pnl2, 2 * 100 * 400);
    // Each trade was two cents from the mid.
    EXPECT_EQ(r.spread2, 2 * (100 * 200) * 2);
    EXPECT_EQ(r.inventory_pnl2, 0);
    EXPECT_EQ(r.max_long, 100);
    EXPECT_EQ(r.max_short, 0);
    EXPECT_EQ(r.max_drawdown2, 0);
    EXPECT_TRUE(r.consistent());
}

TEST_F(SimScenario, APositionIsRevaluedWhenTheMidMoves) {
    start();
    strategy.bid(cents(1001));
    open_market();
    strategy.no_bid();
    tape->execute(b1, 100);  // traded through: we buy 100 at 10.01, mid 10.02
    ASSERT_EQ(report().bought, 100U);
    // b1 has gone, and b2 keeps the real best bid at 10.00.
    EXPECT_EQ(report().spread2, 100 * (kMid2 - twice(100'100)));
    EXPECT_EQ(report().inventory_pnl2, 0);

    tape->remove(a1);  // the offer steps back to 10.10: the mid is now 10.05
    sim::SimReport r = report();
    EXPECT_EQ(r.mid2, 201'000);
    EXPECT_EQ(r.inventory_pnl2, 100 * (201'000 - kMid2));
    EXPECT_EQ(r.pnl2, 100 * (201'000 - 2 * 100'100));
    EXPECT_EQ(r.max_drawdown2, 0);
    EXPECT_TRUE(r.consistent());

    tape->add(Side::Sell, cents(1001), 100);  // and comes in to 10.01: mid 10.005
    r = report();
    EXPECT_EQ(r.mid2, 200'100);
    EXPECT_EQ(r.pnl2, 100 * (200'100 - 200'200));
    EXPECT_LT(r.pnl2, 0);
    // From the high of 80,000 (doubled) down to -10,000.
    EXPECT_EQ(r.max_drawdown2, 100 * (201'000 - 200'100));
    EXPECT_TRUE(r.consistent());
}

TEST_F(SimScenario, ADrawdownIsMeasuredFromAHighThatAFillMade) {
    start();
    const OrderId deeper = tape->add(Side::Buy, cents(999), 70);
    strategy.bid(cents(999));
    open_market();
    const OrderId behind = tape->add(Side::Buy, cents(999), 500);
    tape->remove(deeper);
    tape->remove(b1);  // one real order is left at the best bid
    strategy.no_bid();
    // Bought 100 at 9.99 with the mid at 10.02: up three cents a share at
    // once, and the best prices have not moved.
    tape->execute(behind, 100);
    ASSERT_EQ(report().pnl2, 100 * 600);
    ASSERT_EQ(report().max_drawdown2, 0);

    // The last real bid at 10.00 goes. The best bid is 9.99 and the mid
    // 10.015: the position is worth half a cent a share less. Nothing happened
    // between the fill and this move, so the only record of the high is the
    // one the fill made itself.
    tape->remove(b2);
    const sim::SimReport r = report();
    EXPECT_EQ(r.mid2, 200'300);
    EXPECT_EQ(r.pnl2, 100 * 500);
    EXPECT_EQ(r.max_drawdown2, 100 * 100);
    EXPECT_TRUE(r.consistent());
}

TEST_F(SimScenario, ARebateIsPaidOnEveryShareFilled) {
    sim::SimConfig cfg;
    cfg.rebate = 20;  // a fifth of a cent
    start(cfg);
    strategy.bid(cents(1001));
    open_market();
    strategy.no_bid();
    tape->execute(b1, 100);
    const sim::SimReport r = report();
    EXPECT_EQ(r.rebates, 2'000);
    EXPECT_EQ(r.cash, -100 * static_cast<std::int64_t>(cents(1001)) + 2'000);
    EXPECT_EQ(r.pnl2, r.spread2 + r.inventory_pnl2 + 2 * r.rebates);
    EXPECT_TRUE(r.consistent());
}

TEST_F(SimScenario, BeingShortIsTrackedLikeBeingLong) {
    start();
    strategy.ask(cents(1003));
    open_market();
    strategy.no_ask();
    tape->execute(a1, 60);  // traded through: we sell 60 at 10.03
    sim::SimReport r = report();
    EXPECT_EQ(r.inventory, -60);
    EXPECT_EQ(r.max_short, 60);
    EXPECT_EQ(r.max_long, 0);
    EXPECT_EQ(r.spread2, 60 * (twice(100'300) - kMid2));

    tape->remove(b1);
    tape->remove(b2);  // no bid left: the mid stays where it last was
    r = report();
    EXPECT_EQ(r.mid2, kMid2);
    tape->add(Side::Buy, cents(1003), 10);  // mid 10.035: against a short
    r = report();
    EXPECT_EQ(r.mid2, 200'700);
    EXPECT_EQ(r.inventory_pnl2, -60 * (200'700 - kMid2));
    EXPECT_TRUE(r.consistent());
}

TEST(SimReport, ConsistentMeansTheThreeProfitFiguresAndThePositionAgree) {
    // Bought 300 and sold 200 of something now worth 10.02 a share (doubled:
    // 200,400), with 1,500 of rebates.
    sim::SimReport r;
    r.bought = 300;
    r.sold = 200;
    r.inventory = 100;
    r.mid2 = 200'400;
    r.cash = -1'000'000;
    r.rebates = 1'500;
    r.pnl2 = 2 * r.cash + r.inventory * r.mid2;  // 18,040,000
    r.spread2 = 40'000;
    r.inventory_pnl2 = r.pnl2 - r.spread2 - 2 * r.rebates;
    ASSERT_TRUE(r.consistent());

    // Each figure is tied to the others: move any one and the report no
    // longer agrees with itself.
    sim::SimReport wrong = r;
    wrong.pnl2 += 1;
    EXPECT_FALSE(wrong.consistent()) << "profit";
    wrong = r;
    wrong.cash += 1;
    EXPECT_FALSE(wrong.consistent()) << "cash";
    wrong = r;
    wrong.spread2 += 1;
    EXPECT_FALSE(wrong.consistent()) << "earned at fills";
    wrong = r;
    wrong.inventory_pnl2 -= 1;
    EXPECT_FALSE(wrong.consistent()) << "from holding";
    wrong = r;
    wrong.rebates += 1;
    EXPECT_FALSE(wrong.consistent()) << "rebates";
    wrong = r;
    wrong.sold += 1;
    EXPECT_FALSE(wrong.consistent()) << "shares sold";
    wrong = r;
    wrong.inventory += 1;
    EXPECT_FALSE(wrong.consistent()) << "position";
    EXPECT_TRUE(sim::SimReport{}.consistent()) << "nothing happened is consistent";
}

// --- Markouts --------------------------------------------------------------------

TEST_F(SimScenario, AMarkoutIsTheMidAtTheHorizonAgainstTheFillPrice) {
    start();
    strategy.bid(cents(1001));
    open_market();
    strategy.no_bid();
    tape->execute(b1, 100);  // buy 100 at 10.01
    const Nanos filled = tape->now();

    tape->at(filled + 500'000'000);
    tape->remove(a1);  // mid 10.05 from half a second after the fill
    tape->at(filled + 2'000'000'000);
    tape->add(Side::Sell, cents(1002), 100);  // mid 10.01 from two seconds after
    sim::SimReport r = report();
    // One second after the fill the mid was 10.05.
    EXPECT_EQ(r.markouts[0].horizon, sim::kNanosPerSecond);
    EXPECT_EQ(r.markouts[0].shares, 100U);
    EXPECT_EQ(r.markouts[0].sum2, 100 * (201'000 - 2 * 100'100));
    EXPECT_EQ(r.markouts[1].shares, 0U) << "ten seconds have not passed";

    tape->at(filled + 10'000'000'000);
    ask_again();
    r = report();
    EXPECT_EQ(r.markouts[1].horizon, 10 * sim::kNanosPerSecond);
    EXPECT_EQ(r.markouts[1].shares, 100U);
    // Ten seconds after the fill the mid was 10.01: a cent a share against
    // the 10.02 it was bought near, level with the price paid.
    EXPECT_EQ(r.markouts[1].sum2, 100 * (200'200 - 2 * 100'100));
    sim->finish();
    EXPECT_EQ(sim->report().markouts[0].unmeasured_shares, 0U);
}

TEST_F(SimScenario, AMarkoutForASaleHasTheOppositeSign) {
    start();
    strategy.ask(cents(1003));
    open_market();
    strategy.no_ask();
    tape->execute(a1, 50);  // sell 50 at 10.03
    const Nanos filled = tape->now();
    tape->at(filled + 100);
    tape->remove(a1);  // mid 10.05: the price ran away upwards after we sold
    tape->at(filled + 1'000'000'000);
    ask_again();
    const sim::SimReport r = report();
    EXPECT_EQ(r.markouts[0].shares, 50U);
    EXPECT_EQ(r.markouts[0].sum2, 50 * (2 * 100'300 - 201'000));
    EXPECT_LT(r.markouts[0].sum2, 0);
}

TEST_F(SimScenario, FillsTooCloseToTheEndAreCountedAsUnmeasured) {
    start();
    strategy.bid(cents(1001));
    open_market();
    tape->execute(b1, 40);
    tape->at(tape->now() + 1'500'000'000);
    tape->execute(b1, 30);  // 1.5 seconds after the first fill; fills 30 more
    sim->finish();
    const sim::SimReport r = sim->report();
    ASSERT_EQ(r.bought, 70U);
    EXPECT_EQ(r.markouts[0].shares, 40U);
    EXPECT_EQ(r.markouts[0].unmeasured_shares, 30U);
    EXPECT_EQ(r.markouts[1].shares, 0U);
    EXPECT_EQ(r.markouts[1].unmeasured_shares, 70U);
}

// --- Inventory over time ---------------------------------------------------------

TEST_F(SimScenario, MeanInventoryIsWeightedByHowLongItWasHeld) {
    start();
    strategy.bid(cents(1001));
    open_market();
    strategy.no_bid();
    // The clock for the window starts at the security's first message after
    // its listing, which the tape sends at 3,000. Flat until 1,000,000; long
    // 100 from then until 4,000,000.
    tape->at(1'000'000);
    tape->execute(b1, 100);
    tape->at(4'000'000);
    ask_again();
    const double mean = report().mean_abs_inventory;
    EXPECT_NEAR(mean, 100.0 * 3'000'000.0 / (4'000'000.0 - 3'000.0), 1e-9);
}

}  // namespace
