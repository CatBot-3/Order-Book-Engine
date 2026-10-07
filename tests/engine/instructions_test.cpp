#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "support/engine_harness.hpp"
#include "support/scenario_fixture.hpp"

// Scenario tests for the three instructions an order can carry: post-only,
// iceberg (a display size) and self-match prevention. One rule of
// "Instructions" in obe/engine/concepts.hpp per test, each worked out by hand:
// the reports, the market data and the book that is left.
//
// The ids in these tests are easy to predict and worth predicting, because
// with icebergs they stop being consecutive: every new slice takes one for
// its reference in the market data. Each test says which is which.

namespace {

using namespace obe;
using namespace test::scenario;
using engine::SelfMatch;
using engine::md::add;
using engine::md::execute;
using engine::md::reduce;
using engine::md::remove;
using test::MdMessage;
using test::Report;

template <class Impl>
class Instructions : public ScenarioFixture<Impl> {
 protected:
    OrderId iceberg(Side side, Price price, Qty qty, Qty display, OwnerId owner) {
        return this->submit({.owner = owner,
                             .locate = kStock,
                             .side = side,
                             .qty = qty,
                             .price = price,
                             .display = display});
    }
    OrderId post_only(Side side, Price price, Qty qty, OwnerId owner,
                      SelfMatch mode = SelfMatch::Allow) {
        return this->submit({.owner = owner,
                             .locate = kStock,
                             .side = side,
                             .qty = qty,
                             .price = price,
                             .post_only = true,
                             .self_match = mode});
    }
    OrderId guarded(Side side, Price price, Qty qty, OwnerId owner, SelfMatch mode,
                    TimeInForce tif = TimeInForce::Day) {
        return this->submit({.owner = owner,
                             .locate = kStock,
                             .side = side,
                             .qty = qty,
                             .price = price,
                             .tif = tif,
                             .self_match = mode});
    }

    // Reports for the last request.
    [[nodiscard]] Accepted accepted(OrderId id, OwnerId owner, Side side, Qty qty, Price price,
                                    TimeInForce tif = TimeInForce::Day) const {
        return Accepted{.order_id = id,
                        .owner = owner,
                        .token = this->token,
                        .locate = kStock,
                        .side = side,
                        .qty = qty,
                        .price = price,
                        .kind = OrderKind::Limit,
                        .tif = tif,
                        .timestamp = this->now};
    }
    [[nodiscard]] Cancelled cancelled(OrderId id, OwnerId owner, engine::Token order_token, Qty qty,
                                      CancelReason reason) const {
        return Cancelled{.order_id = id,
                         .owner = owner,
                         .token = order_token,
                         .qty = qty,
                         .leaves = 0,
                         .reason = reason,
                         .timestamp = this->now};
    }
    [[nodiscard]] Rejected rejected(OwnerId owner, engine::Token order_token, OrderId id,
                                    RejectReason reason) const {
        return Rejected{.owner = owner,
                        .token = order_token,
                        .order_id = id,
                        .reason = reason,
                        .timestamp = this->now};
    }
    // The token the n-th order of the test was given (the fixture counts up
    // from 100).
    [[nodiscard]] static engine::Token token_of(std::uint64_t nth) { return 100 + nth; }
};
TYPED_TEST_SUITE(Instructions, ScenarioEngines);

// =============================================================================
// Post-only
// =============================================================================

TYPED_TEST(Instructions, APostOnlyOrderThatWouldNotTradeRestsLikeAnyOther) {
    this->sell(kP + kTick, 100, kBob);
    this->forget();
    const OrderId id = this->post_only(Side::Buy, kP, 300, kAnn);
    EXPECT_EQ(id, 2U);
    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{this->accepted(2, kAnn, Side::Buy, 300, kP)}));
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{add(kStock, this->now, 2, Side::Buy, 300, kSymbol, kP)}));
    ASSERT_EQ(this->bid_orders().size(), 1U);
    const RestingOrder resting = this->bid_orders()[0];
    EXPECT_EQ(resting.qty, 300U);
    EXPECT_TRUE(resting.post_only);  // the instruction stays with the order
    EXPECT_EQ(resting.hidden, 0U);
    EXPECT_EQ(resting.ref, 0U);
}

TYPED_TEST(Instructions, APostOnlyOrderThatWouldTradeIsAcceptedAndThenCancelledWhole) {
    this->sell(kP, 100, kBob);
    // At the ask, and through it: either way it would trade.
    for (const Price price : {kP, kP + 5 * kTick}) {
        this->forget();
        const OrderId id = this->post_only(Side::Buy, price, 300, kAnn);
        ASSERT_NE(id, 0U) << "it is a valid order: it gets an id and an Accepted";
        EXPECT_EQ(this->reports.all,
                  (std::vector<Report>{
                      this->accepted(id, kAnn, Side::Buy, 300, price),
                      this->cancelled(id, kAnn, this->token, 300, CancelReason::PostOnly)}));
        EXPECT_TRUE(this->md.all.empty()) << "the market hears nothing";
        EXPECT_TRUE(this->bids().empty());
        EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 100}})) << "nothing traded";
    }
    const engine::EngineStats stats = this->engine->stats();
    EXPECT_EQ(stats.accepted, 3U);
    EXPECT_EQ(stats.trades, 0U);
    EXPECT_EQ(stats.unfilled_shares, 600U);
}

// "Would trade" is about price alone. Self-match prevention would have kept
// this order from trading with its owner's other order, and that makes no
// difference: prevention is a rule about trades, and post-only is checked
// before there are any.
TYPED_TEST(Instructions, PostOnlyIsJudgedBeforeSelfMatchPreventionIsEvenAsked) {
    this->sell(kP, 100, kAnn);
    this->forget();
    const OrderId id = this->post_only(Side::Buy, kP, 100, kAnn, SelfMatch::CancelResting);
    EXPECT_EQ(
        this->reports.all,
        (std::vector<Report>{this->accepted(id, kAnn, Side::Buy, 100, kP),
                             this->cancelled(id, kAnn, this->token, 100, CancelReason::PostOnly)}));
    EXPECT_TRUE(this->md.all.empty());
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 100}})) << "the resting order is untouched";
    EXPECT_EQ(this->engine->stats().self_matches, 0U);
}

TYPED_TEST(Instructions, PostOnlyOnAnOrderThatCannotRestIsRejected) {
    const NewOrder base{.owner = kAnn,
                        .locate = kStock,
                        .side = Side::Buy,
                        .qty = 100,
                        .price = kP,
                        .post_only = true};
    NewOrder ioc = base;
    ioc.tif = TimeInForce::ImmediateOrCancel;
    NewOrder fok = base;
    fok.tif = TimeInForce::FillOrKill;
    NewOrder market = base;
    market.kind = OrderKind::Market;
    for (const NewOrder& order : {ioc, fok, market}) {
        this->forget();
        EXPECT_EQ(this->submit(order), 0U);
        EXPECT_EQ(this->reports.all, (std::vector<Report>{this->rejected(
                                         kAnn, this->token, 0, RejectReason::BadInstruction)}));
        EXPECT_TRUE(this->md.all.empty());
    }
    // A rejected order uses up no id.
    EXPECT_EQ(this->buy(kP, 100), 1U);
}

TYPED_TEST(Instructions, APostOnlyOrderCannotBeReplacedIntoATrade) {
    const OrderId id = this->post_only(Side::Buy, kP - kTick, 300, kAnn);  // id 1
    this->buy(kP - kTick, 100, kCat);                                      // id 2, behind it
    this->sell(kP, 100, kBob);                                             // id 3
    const engine::EngineStats before = this->engine->stats();
    this->forget();

    // Up to the ask: it would trade, so the replace is refused.
    EXPECT_EQ(this->replace(kAnn, id, 300, kP), 0U);
    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{this->rejected(kAnn, 0, id, RejectReason::WouldTrade)}));
    EXPECT_TRUE(this->md.all.empty());
    // The order is exactly where it was: same id, price, size and place.
    EXPECT_EQ(this->ids(Side::Buy), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(this->bid_orders()[0].qty, 300U);
    EXPECT_EQ(this->bid_orders()[0].price, kP - kTick);
    EXPECT_EQ(this->engine->stats().replaces, before.replaces);
    EXPECT_EQ(this->engine->stats().rejected, before.rejected + 1);

    // A move that would not trade goes through, and the instruction goes with
    // the order: the new one cannot be replaced into a trade either.
    this->forget();
    const OrderId moved = this->replace(kAnn, id, 300, kP - 2 * kTick);
    EXPECT_EQ(moved, 4U);
    ASSERT_EQ(this->bid_orders().size(), 2U);
    EXPECT_EQ(this->bid_orders()[1].id, 4U);
    EXPECT_TRUE(this->bid_orders()[1].post_only);
    EXPECT_EQ(this->replace(kAnn, moved, 300, kP + kTick), 0U);
    EXPECT_EQ(std::get<Rejected>(this->reports.all.back()).reason, RejectReason::WouldTrade);

    // Making it smaller where it is never trades, so it is always allowed.
    EXPECT_EQ(this->replace(kAnn, moved, 50, kP - 2 * kTick), moved);
}

// =============================================================================
// Icebergs
// =============================================================================

TYPED_TEST(Instructions, AnIcebergShowsItsDisplaySizeAndHidesTheRest) {
    const OrderId id = this->iceberg(Side::Sell, kP, 1'000, 200, kBob);
    EXPECT_EQ(id, 1U);
    // The owner is told about all of it; the market about what is shown.
    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{this->accepted(1, kBob, Side::Sell, 1'000, kP)}));
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{add(kStock, this->now, 1, Side::Sell, 200, kSymbol, kP)}));
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 200}}));
    EXPECT_EQ(this->engine->best(kStock, Side::Sell), (Level{kP, 200}));
    EXPECT_EQ(this->engine->open_orders(), 1U);
    EXPECT_EQ(this->ask_orders(), (std::vector<RestingOrder>{{.id = 1,
                                                              .owner = kBob,
                                                              .token = this->token,
                                                              .price = kP,
                                                              .qty = 200,
                                                              .hidden = 800,
                                                              .display = 200}}));
    EXPECT_EQ(this->ask_orders()[0].open(), 1'000U);
    EXPECT_EQ(this->ask_orders()[0].market_ref(), 1U) << "its first slice goes by its own id";
}

TYPED_TEST(Instructions, ADisplaySizeOfZeroOrNotBelowTheOrderShowsAllOfIt) {
    this->iceberg(Side::Sell, kP, 300, 0, kBob);
    this->iceberg(Side::Sell, kP, 300, 300, kBob);
    this->iceberg(Side::Sell, kP, 300, 5'000, kBob);
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 900}}));
    for (const RestingOrder& order : this->ask_orders()) {
        EXPECT_EQ(order.qty, 300U);
        EXPECT_EQ(order.hidden, 0U);
    }
    // The size it was given stays with it, used or not.
    EXPECT_EQ(this->ask_orders()[0].display, 0U);
    EXPECT_EQ(this->ask_orders()[1].display, 300U);
    EXPECT_EQ(this->ask_orders()[2].display, 5'000U);
}

// The central case. Bob's iceberg (500, showing 200) is first in the queue at
// kP and Cat's plain 100 is behind it. Ann buys 250.
//   1. 200 with Bob's slice. Nothing of his shows any more, so the next slice
//      comes up: 200 more, under a NEW reference (4: ids 1 to 3 are the three
//      orders), at the BACK of the queue.
//   2. 50 with Cat, who is now first.
TYPED_TEST(Instructions, TradingThroughASliceBringsTheNextOneUpBehindTheQueue) {
    this->iceberg(Side::Sell, kP, 500, 200, kBob);  // id 1, token 101
    this->sell(kP, 100, kCat);                      // id 2, token 102
    this->forget();
    const OrderId id = this->buy(kP, 250, kAnn);  // id 3, token 103
    const Nanos t = this->now;
    EXPECT_EQ(id, 3U);

    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{execute(kStock, t, 1, 200, 1),
                                                    add(kStock, t, 4, Side::Sell, 200, kSymbol, kP),
                                                    execute(kStock, t, 2, 50, 2)}));
    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{
                  this->accepted(3, kAnn, Side::Buy, 250, kP),
                  // Bob's report is under his order's own id, and counts what
                  // is hidden: 300 of his 500 are still open.
                  this->fill(1, kBob, this->token_of(1), 200, kP, 300, 1, Liquidity::Added),
                  this->fill(3, kAnn, this->token_of(3), 200, kP, 50, 1, Liquidity::Removed),
                  this->fill(2, kCat, this->token_of(2), 50, kP, 50, 2, Liquidity::Added),
                  this->fill(3, kAnn, this->token_of(3), 50, kP, 0, 2, Liquidity::Removed)}));

    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 250}}));  // Cat's 50, Bob's new 200
    EXPECT_EQ(this->ask_orders(),
              (std::vector<RestingOrder>{
                  {.id = 2, .owner = kCat, .token = this->token_of(2), .price = kP, .qty = 50},
                  {.id = 1,
                   .owner = kBob,
                   .token = this->token_of(1),
                   .price = kP,
                   .qty = 200,
                   .hidden = 100,
                   .display = 200,
                   .ref = 4}}));
    EXPECT_EQ(this->engine->open_orders(), 2U);
    // The slice took id 4, so the next order is 5.
    EXPECT_EQ(this->buy(kP - kTick, 100, kAnn), 5U);
}

// An order bigger than everything showing at a level meets the same iceberg
// again and again, a slice at a time, each one a separate trade.
TYPED_TEST(Instructions, AnOrderLargerThanTheSliceMeetsTheIcebergAgain) {
    this->iceberg(Side::Sell, kP, 500, 200, kBob);  // id 1
    this->forget();
    this->buy(kP, 450, kAnn);  // id 2; the slices take 3 and 4
    const Nanos t = this->now;

    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{
                  execute(kStock, t, 1, 200, 1), add(kStock, t, 3, Side::Sell, 200, kSymbol, kP),
                  execute(kStock, t, 3, 200, 2),
                  // The last slice is what is left: 100.
                  add(kStock, t, 4, Side::Sell, 100, kSymbol, kP), execute(kStock, t, 4, 50, 3)}));
    std::vector<Qty> bob_leaves;
    for (const Executed& e : this->reports.template of<Executed>()) {
        if (e.owner == kBob) {
            EXPECT_EQ(e.order_id, 1U) << "always his order's id, whatever the market calls it";
            bob_leaves.push_back(e.leaves);
        }
    }
    EXPECT_EQ(bob_leaves, (std::vector<Qty>{300, 100, 50}));
    EXPECT_EQ(this->ask_orders(), (std::vector<RestingOrder>{{.id = 1,
                                                              .owner = kBob,
                                                              .token = this->token_of(1),
                                                              .price = kP,
                                                              .qty = 50,
                                                              .hidden = 0,
                                                              .display = 200,
                                                              .ref = 4}}));
    EXPECT_TRUE(this->bids().empty());
}

TYPED_TEST(Instructions, AnIcebergTradedOutCompletelyIsGoneWithoutAnotherWord) {
    this->iceberg(Side::Sell, kP, 250, 100, kBob);  // id 1
    this->forget();
    this->buy(kP, 250, kAnn);  // id 2; slices 3 and 4
    const Nanos t = this->now;
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{
                  execute(kStock, t, 1, 100, 1), add(kStock, t, 3, Side::Sell, 100, kSymbol, kP),
                  execute(kStock, t, 3, 100, 2), add(kStock, t, 4, Side::Sell, 50, kSymbol, kP),
                  execute(kStock, t, 4, 50, 3)}));
    EXPECT_TRUE(this->asks().empty());
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->engine->open_orders(), 0U);
    EXPECT_EQ(this->engine->best(kStock, Side::Sell), std::nullopt);
}

// The slice behind the queue has lost its place, and the next buyer shows it.
TYPED_TEST(Instructions, ANewSliceWaitsBehindOrdersThatWereBehindTheOldOne) {
    this->iceberg(Side::Sell, kP, 300, 100, kBob);  // id 1
    this->sell(kP, 100, kCat);                      // id 2
    EXPECT_EQ(this->ids(Side::Sell), (std::vector<OrderId>{1, 2}));
    this->buy(kP, 100, kAnn);  // id 3 takes Bob's slice; the new one is ref 4
    EXPECT_EQ(this->ids(Side::Sell), (std::vector<OrderId>{2, 1}));
    this->forget();
    this->buy(kP, 100, kAnn);  // id 5
    // Cat, not Bob.
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{execute(kStock, this->now, 2, 100, 2)}));
    EXPECT_EQ(this->ids(Side::Sell), (std::vector<OrderId>{1}));
}

// The display size limits what an iceberg shows while it rests. It does not
// limit what it takes when it arrives.
TYPED_TEST(Instructions, AnArrivingIcebergTradesWithItsWholeSizeAndThenShowsASlice) {
    this->buy(kP, 300, kAnn);  // id 1
    this->forget();
    this->iceberg(Side::Sell, kP, 1'000, 200, kBob);  // id 2
    const Nanos t = this->now;
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{execute(kStock, t, 1, 300, 1),
                                      add(kStock, t, 2, Side::Sell, 200, kSymbol, kP)}));
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->ask_orders(), (std::vector<RestingOrder>{{.id = 2,
                                                              .owner = kBob,
                                                              .token = this->token,
                                                              .price = kP,
                                                              .qty = 200,
                                                              .hidden = 500,
                                                              .display = 200}}));
}

TYPED_TEST(Instructions, CancellingAnIcebergNamesTheSliceTheMarketSeesAndReturnsEverything) {
    this->iceberg(Side::Sell, kP, 500, 200, kBob);  // id 1
    this->buy(kP, 200, kAnn);                       // id 2; Bob's next slice is ref 3
    this->forget();

    // The reference is the market's name for a slice, not an order id.
    EXPECT_FALSE(this->cancel(kBob, 3));
    EXPECT_EQ(std::get<Rejected>(this->reports.all.back()).reason, RejectReason::UnknownOrder);
    this->forget();

    EXPECT_TRUE(this->cancel(kBob, 1));
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{remove(kStock, this->now, 3)}));
    EXPECT_EQ(this->reports.all, (std::vector<Report>{this->cancelled(
                                     1, kBob, this->token_of(1), 300, CancelReason::Requested)}));
    EXPECT_TRUE(this->asks().empty());
    EXPECT_EQ(this->engine->open_orders(), 0U);
}

// Making an iceberg smaller where it stands takes from the part nobody can
// see first. The market is told only when the part it can see shrinks.
TYPED_TEST(Instructions, ReducingAnIcebergTakesFromTheHiddenPartFirst) {
    this->iceberg(Side::Sell, kP, 1'000, 200, kBob);  // id 1
    this->forget();

    EXPECT_EQ(this->replace(kBob, 1, 700, kP), 1U);
    EXPECT_TRUE(this->md.all.empty()) << "300 hidden shares went; nothing the market saw changed";
    ASSERT_EQ(this->reports.template of<Replaced>().size(), 1U);
    EXPECT_TRUE(this->reports.template of<Replaced>()[0].kept_priority);
    EXPECT_EQ(this->reports.template of<Replaced>()[0].qty, 700U);
    EXPECT_EQ(this->ask_orders()[0].qty, 200U);
    EXPECT_EQ(this->ask_orders()[0].hidden, 500U);
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 200}}));

    // Down to 150: the remaining 500 hidden go, and 50 of the 200 shown.
    this->forget();
    EXPECT_EQ(this->replace(kBob, 1, 150, kP), 1U);
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{reduce(kStock, this->now, 1, 50)}));
    EXPECT_EQ(this->ask_orders()[0].qty, 150U);
    EXPECT_EQ(this->ask_orders()[0].hidden, 0U);
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 150}}));

    // The same size again is a replace that changes nothing.
    this->forget();
    EXPECT_EQ(this->replace(kBob, 1, 150, kP), 1U);
    EXPECT_TRUE(this->md.all.empty());
}

TYPED_TEST(Instructions, AReductionAfterANewSliceNamesThatSlice) {
    this->iceberg(Side::Sell, kP, 500, 200, kBob);  // id 1
    this->buy(kP, 200, kAnn);                       // id 2; Bob now shows 200 as ref 3, hides 100
    this->forget();
    EXPECT_EQ(this->replace(kBob, 1, 120, kP), 1U);  // 100 hidden go, then 80 shown
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{reduce(kStock, this->now, 3, 80)}));
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 120}}));
}

TYPED_TEST(Instructions, AReplacedIcebergIsANewIcebergWithTheSameDisplaySize) {
    this->iceberg(Side::Sell, kP, 500, 200, kBob);  // id 1
    this->buy(kP, 200, kAnn);                       // id 2; Bob's slice is now ref 3
    this->forget();

    // A new price: a new order, id 4. The market sees the slice it knew (3)
    // replaced by one of 200; the other 400 are hidden again.
    EXPECT_EQ(this->replace(kBob, 1, 600, kP + kTick), 4U);
    EXPECT_EQ(
        this->md.all,
        (std::vector<MdMessage>{engine::md::replace(kStock, this->now, 3, 4, 200, kP + kTick)}));
    ASSERT_EQ(this->reports.template of<Replaced>().size(), 1U);
    EXPECT_FALSE(this->reports.template of<Replaced>()[0].kept_priority);
    EXPECT_EQ(this->reports.template of<Replaced>()[0].qty, 600U) << "all of it, shown or not";
    EXPECT_EQ(this->ask_orders(), (std::vector<RestingOrder>{{.id = 4,
                                                              .owner = kBob,
                                                              .token = this->token_of(1),
                                                              .price = kP + kTick,
                                                              .qty = 200,
                                                              .hidden = 400,
                                                              .display = 200}}));

    // Small enough to show in full, it shows in full, and keeps the display
    // size for the day it grows again.
    this->forget();
    EXPECT_EQ(this->replace(kBob, 4, 150, kP + 2 * kTick), 5U);
    EXPECT_EQ(this->ask_orders()[0].qty, 150U);
    EXPECT_EQ(this->ask_orders()[0].hidden, 0U);
    EXPECT_EQ(this->replace(kBob, 5, 900, kP + 2 * kTick), 6U);  // larger: to the back, id 6
    EXPECT_EQ(this->ask_orders()[0].qty, 200U);
    EXPECT_EQ(this->ask_orders()[0].hidden, 700U);
}

TYPED_TEST(Instructions, FillOrKillCountsSharesItCannotSee) {
    this->iceberg(Side::Sell, kP, 500, 100, kBob);  // id 1
    this->forget();

    // One more than there is: killed, and nothing at all has happened.
    const OrderId killed = this->buy(kP, 501, kAnn, TimeInForce::FillOrKill);  // id 2
    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{
                  this->accepted(killed, kAnn, Side::Buy, 501, kP, TimeInForce::FillOrKill),
                  this->cancelled(killed, kAnn, this->token, 501, CancelReason::FillOrKill)}));
    EXPECT_TRUE(this->md.all.empty());
    EXPECT_EQ(this->ask_orders()[0].hidden, 400U);

    // Exactly what there is, though only 100 are showing: filled, five trades.
    this->forget();
    this->buy(kP, 500, kAnn, TimeInForce::FillOrKill);  // id 3
    EXPECT_EQ(this->md.template of<feed::OrderExecuted>().size(), 5U);
    EXPECT_EQ(this->md.template of<feed::AddOrder>().size(), 4U);
    EXPECT_TRUE(this->asks().empty());
    EXPECT_EQ(this->engine->stats().traded_shares, 500U);
}

// The reserve a fill-or-kill may count on is the reserve as it is now. Every
// new slice, every reduction and every cancel changes it, and a count that is
// kept on the side has to change with it. Each step below leaves the true
// amount one way and a stale count another; the order sized between the two
// must be killed.
TYPED_TEST(Instructions, FillOrKillCountsTheReserveAsItIsNowAndNotAsItWas) {
    const auto killed = [this](Qty qty) {
        this->forget();
        const OrderId id = this->buy(kP, qty, kAnn, TimeInForce::FillOrKill);
        const std::vector<Cancelled> cancels = this->reports.template of<Cancelled>();
        const bool was = cancels.size() == 1 && cancels[0].order_id == id &&
                         cancels[0].reason == CancelReason::FillOrKill;
        EXPECT_EQ(was, this->md.all.empty()) << "a kill is silent, a fill is not";
        return was;
    };
    const auto open_at_kp = [this] {
        std::uint64_t total = 0;
        for (const RestingOrder& order : this->ask_orders()) {
            if (order.price == kP) {
                total += order.open();
            }
        }
        return total;
    };

    // After a slice has come up: 500 less the 100 that traded.
    this->iceberg(Side::Sell, kP, 500, 100, kBob);
    this->buy(kP, 100, kCat);
    ASSERT_EQ(open_at_kp(), 400U);
    EXPECT_TRUE(killed(401));
    EXPECT_TRUE(killed(500));

    // After a reduction that took only hidden shares.
    ASSERT_EQ(this->replace(kBob, 1, 250, kP), 1U);
    ASSERT_EQ(open_at_kp(), 250U);
    EXPECT_TRUE(killed(251));
    EXPECT_TRUE(killed(400));

    // After a second iceberg at the price was cancelled.
    const OrderId other = this->iceberg(Side::Sell, kP, 600, 50, kCat);
    ASSERT_EQ(open_at_kp(), 850U);
    ASSERT_TRUE(this->cancel(kCat, other));
    ASSERT_EQ(open_at_kp(), 250U);
    EXPECT_TRUE(killed(251));
    EXPECT_TRUE(killed(850));

    // After one was replaced away to another price, and one arrived by replace.
    const OrderId third = this->iceberg(Side::Sell, kP, 300, 50, kCat);
    ASSERT_NE(this->replace(kCat, third, 300, kP + 5 * kTick), 0U);
    ASSERT_EQ(open_at_kp(), 250U);
    EXPECT_TRUE(killed(251));
    const OrderId fourth = this->iceberg(Side::Sell, kP + 9 * kTick, 200, 20, kCat);
    ASSERT_NE(this->replace(kCat, fourth, 200, kP), 0U);
    ASSERT_EQ(open_at_kp(), 450U);
    EXPECT_TRUE(killed(451));

    // After an iceberg was taken out by its owner's own order. Bob's is first
    // in the queue, so his buy meets it, cancels all 250 of it, and trades 10
    // with the other.
    this->guarded(Side::Buy, kP, 10, kBob, SelfMatch::CancelResting);
    ASSERT_EQ(open_at_kp(), 190U);
    EXPECT_TRUE(killed(191));
    EXPECT_TRUE(killed(340));

    // And exactly what is there fills, to the last hidden share.
    EXPECT_FALSE(killed(190));
    EXPECT_EQ(open_at_kp(), 0U);
}

TYPED_TEST(Instructions, ADisplaySizeOnAnOrderThatCannotRestIsRejectedIfItWouldHideAnything) {
    NewOrder order{.owner = kAnn,
                   .locate = kStock,
                   .side = Side::Buy,
                   .qty = 300,
                   .price = kP,
                   .tif = TimeInForce::ImmediateOrCancel,
                   .display = 100};
    EXPECT_EQ(this->submit(order), 0U);
    EXPECT_EQ(std::get<Rejected>(this->reports.all.back()).reason, RejectReason::BadInstruction);
    order.kind = OrderKind::Market;
    order.tif = TimeInForce::Day;
    EXPECT_EQ(this->submit(order), 0U);
    EXPECT_EQ(std::get<Rejected>(this->reports.all.back()).reason, RejectReason::BadInstruction);

    // A display size that hides nothing is no instruction at all.
    order.kind = OrderKind::Limit;
    order.tif = TimeInForce::ImmediateOrCancel;
    order.display = 300;
    EXPECT_EQ(this->submit(order), 1U);
    order.display = 0;
    EXPECT_EQ(this->submit(order), 2U);
}

// =============================================================================
// Self-match prevention
// =============================================================================

TYPED_TEST(Instructions, CancelIncomingStopsTheArrivingOrderAndLeavesTheRestingOne) {
    this->sell(kP, 100, kAnn);  // id 1
    this->forget();
    const OrderId id = this->guarded(Side::Buy, kP, 300, kAnn, SelfMatch::CancelIncoming);
    EXPECT_EQ(id, 2U);
    // A day order, and still nothing of it rests: the instruction says stop.
    EXPECT_EQ(
        this->reports.all,
        (std::vector<Report>{this->accepted(2, kAnn, Side::Buy, 300, kP),
                             this->cancelled(2, kAnn, this->token, 300, CancelReason::SelfMatch)}));
    EXPECT_TRUE(this->md.all.empty());
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 100}}));
    const engine::EngineStats stats = this->engine->stats();
    EXPECT_EQ(stats.self_matches, 1U);
    EXPECT_EQ(stats.unfilled_shares, 300U);
    EXPECT_EQ(stats.trades, 0U);
    EXPECT_EQ(stats.cancels, 0U) << "nobody asked for a cancel";
}

TYPED_TEST(Instructions, CancelIncomingTradesWithOthersUntilItMeetsItsOwnOrder) {
    this->sell(kP, 100, kBob);  // id 1
    this->sell(kP, 100, kAnn);  // id 2
    this->sell(kP, 100, kCat);  // id 3, behind Ann's and never reached
    this->forget();
    this->guarded(Side::Buy, kP, 300, kAnn, SelfMatch::CancelIncoming);  // id 4
    const Nanos t = this->now;
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{execute(kStock, t, 1, 100, 1)}));
    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{
                  this->accepted(4, kAnn, Side::Buy, 300, kP),
                  this->fill(1, kBob, this->token_of(1), 100, kP, 0, 1, Liquidity::Added),
                  this->fill(4, kAnn, this->token_of(4), 100, kP, 200, 1, Liquidity::Removed),
                  this->cancelled(4, kAnn, this->token_of(4), 200, CancelReason::SelfMatch)}));
    EXPECT_EQ(this->ids(Side::Sell), (std::vector<OrderId>{2, 3}));
}

TYPED_TEST(Instructions, CancelRestingRemovesTheOwnersOrderAndCarriesOn) {
    this->sell(kP, 100, kAnn);  // id 1
    this->sell(kP, 100, kBob);  // id 2
    this->forget();
    this->guarded(Side::Buy, kP, 300, kAnn, SelfMatch::CancelResting);  // id 3
    const Nanos t = this->now;
    // Ann's resting order is deleted, Bob's is traded with, and the 200 left
    // of the arriving order rest: it is a day order and nothing stopped it.
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{remove(kStock, t, 1), execute(kStock, t, 2, 100, 1),
                                      add(kStock, t, 3, Side::Buy, 200, kSymbol, kP)}));
    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{
                  this->accepted(3, kAnn, Side::Buy, 300, kP),
                  this->cancelled(1, kAnn, this->token_of(1), 100, CancelReason::SelfMatch),
                  this->fill(2, kBob, this->token_of(2), 100, kP, 0, 1, Liquidity::Added),
                  this->fill(3, kAnn, this->token_of(3), 100, kP, 200, 1, Liquidity::Removed)}));
    EXPECT_TRUE(this->asks().empty());
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP, 200}}));
    // The instruction rests with the order.
    EXPECT_EQ(this->bid_orders()[0].self_match, SelfMatch::CancelResting);
    const engine::EngineStats stats = this->engine->stats();
    EXPECT_EQ(stats.self_matches, 1U);
    EXPECT_EQ(stats.unfilled_shares, 0U);
    EXPECT_EQ(stats.cancels, 0U);
}

TYPED_TEST(Instructions, CancelBothRemovesTheRestingOrderAndStopsTheArrivingOne) {
    this->sell(kP, 100, kAnn);  // id 1
    this->sell(kP, 100, kBob);  // id 2
    this->forget();
    this->guarded(Side::Buy, kP, 300, kAnn, SelfMatch::CancelBoth);  // id 3
    const Nanos t = this->now;
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{remove(kStock, t, 1)}));
    // The resting order first, then the arriving one.
    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{
                  this->accepted(3, kAnn, Side::Buy, 300, kP),
                  this->cancelled(1, kAnn, this->token_of(1), 100, CancelReason::SelfMatch),
                  this->cancelled(3, kAnn, this->token_of(3), 300, CancelReason::SelfMatch)}));
    EXPECT_EQ(this->ids(Side::Sell), (std::vector<OrderId>{2})) << "Bob was never reached";
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->engine->stats().self_matches, 1U) << "one meeting, however many cancels";
    EXPECT_EQ(this->engine->stats().unfilled_shares, 300U);
}

// Prevention is about the order that would trade NEXT. An order of the same
// owner further down the book, never reached, changes nothing.
TYPED_TEST(Instructions, PreventionOnlyActsOnTheOrderItIsAboutToTradeWith) {
    this->sell(kP, 300, kBob);          // id 1: enough for all three buys below
    this->sell(kP + kTick, 100, kAnn);  // id 2, a worse price
    for (const SelfMatch mode :
         {SelfMatch::CancelIncoming, SelfMatch::CancelResting, SelfMatch::CancelBoth}) {
        this->forget();
        // Filled by Bob before Ann's own order is ever best.
        this->guarded(Side::Buy, kP + kTick, 50, kAnn, mode, TimeInForce::ImmediateOrCancel);
        EXPECT_EQ(this->md.template of<feed::OrderExecuted>().size(), 1U);
        EXPECT_TRUE(this->reports.template of<Cancelled>().empty());
    }
    EXPECT_EQ(this->engine->stats().self_matches, 0U);
    EXPECT_EQ(this->ids(Side::Sell), (std::vector<OrderId>{1, 2}));
}

TYPED_TEST(Instructions, AMarketOrderWithCancelRestingSweepsItsOwnersOrdersAway) {
    this->sell(kP, 100, kAnn);         // id 1
    this->sell(kP + kTick, 50, kAnn);  // id 2
    this->forget();
    this->submit({.owner = kAnn,
                  .locate = kStock,
                  .side = Side::Buy,
                  .qty = 100,
                  .kind = OrderKind::Market,
                  .self_match = SelfMatch::CancelResting});  // id 3
    const Nanos t = this->now;
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{remove(kStock, t, 1), remove(kStock, t, 2)}));
    const std::vector<Cancelled> cancels = this->reports.template of<Cancelled>();
    ASSERT_EQ(cancels.size(), 3U);
    EXPECT_EQ(cancels[0],
              this->cancelled(1, kAnn, this->token_of(1), 100, CancelReason::SelfMatch));
    EXPECT_EQ(cancels[1], this->cancelled(2, kAnn, this->token_of(2), 50, CancelReason::SelfMatch));
    // Then there is nobody left to trade with, which is its own reason.
    EXPECT_EQ(cancels[2],
              this->cancelled(3, kAnn, this->token_of(3), 100, CancelReason::NoLiquidity));
    EXPECT_EQ(this->engine->stats().self_matches, 2U);
    EXPECT_EQ(this->engine->stats().unfilled_shares, 100U);
    EXPECT_EQ(this->engine->open_orders(), 0U);
}

TYPED_TEST(Instructions, CancelRestingTakesAnIcebergOutWhole) {
    this->iceberg(Side::Sell, kP, 500, 100, kAnn);  // id 1
    this->forget();
    this->guarded(Side::Buy, kP, 50, kAnn, SelfMatch::CancelResting);  // id 2
    const Nanos t = this->now;
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{remove(kStock, t, 1),
                                      add(kStock, t, 2, Side::Buy, 50, kSymbol, kP)}));
    const std::vector<Cancelled> cancels = this->reports.template of<Cancelled>();
    ASSERT_EQ(cancels.size(), 1U);
    EXPECT_EQ(cancels[0].qty, 500U) << "what was hidden as well as what showed";
    EXPECT_TRUE(this->asks().empty());
}

TYPED_TEST(Instructions, AValueThatIsNotOneOfTheThreeModesMeansAllow) {
    this->sell(kP, 100, kAnn);
    this->forget();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    this->guarded(Side::Buy, kP, 100, kAnn, static_cast<SelfMatch>(9));
    EXPECT_EQ(this->md.template of<feed::OrderExecuted>().size(), 1U) << "it traded with itself";
    EXPECT_EQ(this->engine->stats().self_matches, 0U);
    // And Allow itself, to be plain about what the default is.
    this->sell(kP, 100, kAnn);
    this->guarded(Side::Buy, kP, 100, kAnn, SelfMatch::Allow);
    EXPECT_EQ(this->engine->stats().trades, 2U);
}

// The instruction was given when the order arrived and is still in force when
// a replace sends it across the book weeks later.
TYPED_TEST(Instructions, TheInstructionAppliesWhenAReplaceMakesARestingOrderTrade) {
    const OrderId id = this->guarded(Side::Buy, kP - kTick, 100, kAnn, SelfMatch::CancelIncoming);
    this->sell(kP, 100, kAnn);  // id 2: her own, at a price the buy will be moved to
    this->forget();
    const OrderId moved = this->replace(kAnn, id, 100, kP);
    const Nanos t = this->now;
    EXPECT_EQ(moved, 3U);
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{remove(kStock, t, 1)}));
    ASSERT_EQ(this->reports.all.size(), 2U);
    EXPECT_FALSE(std::get<Replaced>(this->reports.all[0]).kept_priority);
    EXPECT_EQ(this->reports.all[1],
              Report{this->cancelled(3, kAnn, this->token_of(1), 100, CancelReason::SelfMatch)});
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 100}}));
    EXPECT_EQ(this->engine->stats().self_matches, 1U);
}

// --- Fill-or-kill has to know in advance ------------------------------------

// Bob 100, Ann 100, Cat 100 at one price, in that order. What can Ann's
// fill-or-kill buy count on?
TYPED_TEST(Instructions, FillOrKillCountsOnlyWhatPreventionWouldLetItReach) {
    this->sell(kP, 100, kBob);  // id 1
    this->sell(kP, 100, kAnn);  // id 2
    this->sell(kP, 100, kCat);  // id 3
    const auto fok = [this](Qty qty, SelfMatch mode) {
        this->forget();
        const OrderId id = this->guarded(Side::Buy, kP, qty, kAnn, mode, TimeInForce::FillOrKill);
        const std::vector<Cancelled> cancels = this->reports.template of<Cancelled>();
        const bool killed = cancels.size() == 1 && cancels[0].order_id == id &&
                            cancels[0].reason == CancelReason::FillOrKill;
        if (killed) {
            // A kill is only a kill if nothing else happened.
            EXPECT_TRUE(this->md.all.empty());
            EXPECT_EQ(this->reports.all.size(), 2U);
        }
        return killed;
    };

    // Stopping at her own order, she reaches Bob's 100 and no further.
    EXPECT_TRUE(fok(101, SelfMatch::CancelIncoming));
    EXPECT_TRUE(fok(101, SelfMatch::CancelBoth));
    // Cancelling her own order, she reaches Bob and Cat: 200.
    EXPECT_TRUE(fok(201, SelfMatch::CancelResting));
    EXPECT_EQ(this->ids(Side::Sell), (std::vector<OrderId>{1, 2, 3}))
        << "a killed order cancels nothing but itself";
    // Trading with herself, all 300.
    EXPECT_TRUE(fok(301, SelfMatch::Allow));
    EXPECT_EQ(this->engine->stats().self_matches, 0U);

    // And one that fits: 200 with her own order cancelled on the way.
    EXPECT_FALSE(fok(200, SelfMatch::CancelResting));
    const Nanos t = this->now;
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{execute(kStock, t, 1, 100, 1), remove(kStock, t, 2),
                                      execute(kStock, t, 3, 100, 2)}));
    EXPECT_TRUE(this->asks().empty());
}

// The subtle one. Bob's iceberg (400, showing 100) is ahead of Ann's own
// order. Ann's fill-or-kill buy of 200 stops at her own order.
//
// It is tempting to count Bob's 400. But after his first 100 trade, his next
// slice comes up BEHIND Ann's order, and the arriving order meets hers first
// and stops. Only the 100 that are showing ahead of her can be reached.
TYPED_TEST(Instructions, HiddenSharesBehindTheOwnersOrderCannotBeCountedOn) {
    this->iceberg(Side::Sell, kP, 400, 100, kBob);  // id 1
    this->sell(kP, 100, kAnn);                      // id 2
    this->forget();
    const OrderId id = this->guarded(Side::Buy, kP, 200, kAnn, SelfMatch::CancelIncoming,
                                     TimeInForce::FillOrKill);  // id 3
    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{
                  this->accepted(id, kAnn, Side::Buy, 200, kP, TimeInForce::FillOrKill),
                  this->cancelled(id, kAnn, this->token, 200, CancelReason::FillOrKill)}));
    EXPECT_TRUE(this->md.all.empty());

    // 100 does fit.
    this->forget();
    this->guarded(Side::Buy, kP, 100, kAnn, SelfMatch::CancelIncoming, TimeInForce::FillOrKill);
    EXPECT_EQ(this->md.template of<feed::OrderExecuted>().size(), 1U);
    EXPECT_TRUE(this->reports.template of<Cancelled>().empty());
}

// With CancelResting the same book fills 200: her order is cancelled when it
// is reached, and the iceberg's next slice is right behind it.
TYPED_TEST(Instructions, WithCancelRestingTheHiddenSharesAreReachedAfterTheOwnersOrderGoes) {
    this->iceberg(Side::Sell, kP, 400, 100, kBob);  // id 1
    this->sell(kP, 100, kAnn);                      // id 2
    this->forget();
    this->guarded(Side::Buy, kP, 200, kAnn, SelfMatch::CancelResting,
                  TimeInForce::FillOrKill);  // id 3; Bob's slices take 4 and 5
    const Nanos t = this->now;
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{execute(kStock, t, 1, 100, 1),
                                      add(kStock, t, 4, Side::Sell, 100, kSymbol, kP),
                                      remove(kStock, t, 2), execute(kStock, t, 4, 100, 2),
                                      add(kStock, t, 5, Side::Sell, 100, kSymbol, kP)}));
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 100}}));
    EXPECT_EQ(this->ask_orders()[0].hidden, 100U);
    EXPECT_EQ(this->engine->stats().self_matches, 1U);
}

}  // namespace
