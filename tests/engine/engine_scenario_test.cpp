#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "support/engine_harness.hpp"
#include "support/naive_engine.hpp"

// Scenario tests: one for each order type and each edge the contract in
// obe/engine/concepts.hpp describes. They assume only that contract, and check
// all three things an operation produces: the reports to the owners, the
// market data, and the book that is left.
//
// In the passing suite they run against the reference engine and against the
// test oracle (support/naive_engine.hpp). The property tests use the oracle to
// judge the reference on random flow, which is only worth something if the
// oracle itself agrees with these hand-worked cases.

namespace {

using namespace obe;
using book::Level;
using engine::Accepted;
using engine::Cancelled;
using engine::CancelReason;
using engine::Executed;
using engine::Liquidity;
using engine::NewOrder;
using engine::OrderKind;
using engine::OwnerId;
using engine::Rejected;
using engine::RejectReason;
using engine::Replaced;
using engine::RestingOrder;
using engine::TimeInForce;
using test::MdMessage;
using test::Report;

constexpr Locate kStock = 7;
constexpr Locate kOther = 8;
constexpr Locate kUnopened = 9;
constexpr OwnerId kAnn = 1;
constexpr OwnerId kBob = 2;
constexpr OwnerId kCat = 3;
constexpr Price kP = 1'000'000;  // $100.00
constexpr Price kTick = 100;     // one cent
constexpr feed::Symbol kSymbol = feed::Symbol::from("ACME");
// A side byte that is neither 'B' nor 'S', as a corrupt or hostile request
// would carry. The cast is well defined: Side has a fixed underlying type.
// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
constexpr Side kBadSide = static_cast<Side>('X');

template <class Impl>
class EngineScenario : public ::testing::Test {
 protected:
    using Engine = typename Impl::template Engine<test::ReportLog, test::MdLog>;

    void SetUp() override {
        ASSERT_TRUE(engine->add_instrument(kStock, kSymbol, tick()));
        ASSERT_TRUE(engine->add_instrument(kOther, feed::Symbol::from("OTHR"), tick()));
        forget();
    }

    // Each request gets its own time, so a test can tell which request an
    // output belongs to.
    Nanos tick() { return now += 1000; }

    // Empties the logs, so a test sees only what its last step produced.
    void forget() {
        reports.clear();
        md.clear();
    }

    OrderId submit(NewOrder order) {
        order.token = ++token;
        return engine->submit(order, tick());
    }
    OrderId buy(Price price, Qty qty, OwnerId owner = kAnn, TimeInForce tif = TimeInForce::Day) {
        return submit({.owner = owner,
                       .locate = kStock,
                       .side = Side::Buy,
                       .qty = qty,
                       .price = price,
                       .tif = tif});
    }
    OrderId sell(Price price, Qty qty, OwnerId owner = kBob, TimeInForce tif = TimeInForce::Day) {
        return submit({.owner = owner,
                       .locate = kStock,
                       .side = Side::Sell,
                       .qty = qty,
                       .price = price,
                       .tif = tif});
    }
    OrderId market(Side side, Qty qty, OwnerId owner = kCat, TimeInForce tif = TimeInForce::Day) {
        return submit({.owner = owner,
                       .locate = kStock,
                       .side = side,
                       .qty = qty,
                       .kind = OrderKind::Market,
                       .tif = tif});
    }
    bool cancel(OwnerId owner, OrderId id) { return engine->cancel(owner, id, tick()); }
    OrderId replace(OwnerId owner, OrderId id, Qty qty, Price price) {
        return engine->replace(owner, id, qty, price, tick());
    }

    [[nodiscard]] std::vector<Level> bids() const {
        return test::levels_of(*engine, kStock, Side::Buy);
    }
    [[nodiscard]] std::vector<Level> asks() const {
        return test::levels_of(*engine, kStock, Side::Sell);
    }
    [[nodiscard]] std::vector<RestingOrder> bid_orders() const {
        return test::orders_of(*engine, kStock, Side::Buy);
    }
    [[nodiscard]] std::vector<RestingOrder> ask_orders() const {
        return test::orders_of(*engine, kStock, Side::Sell);
    }
    // The ids of one side, in the order they would trade.
    [[nodiscard]] std::vector<OrderId> ids(Side side) const {
        std::vector<OrderId> out;
        for (const RestingOrder& o : test::orders_of(*engine, kStock, side)) {
            out.push_back(o.id);
        }
        return out;
    }

    // An Executed report for the last request.
    [[nodiscard]] Executed fill(OrderId id, OwnerId owner, engine::Token order_token, Qty qty,
                                Price price, Qty leaves, std::uint64_t match,
                                Liquidity liquidity) const {
        return Executed{.order_id = id,
                        .owner = owner,
                        .token = order_token,
                        .qty = qty,
                        .price = price,
                        .leaves = leaves,
                        .match_number = match,
                        .liquidity = liquidity,
                        .timestamp = now};
    }

    test::ReportLog reports;
    test::MdLog md;
    std::unique_ptr<Engine> engine = std::make_unique<Engine>(reports, md);
    Nanos now = 0;
    engine::Token token = 100;
};
#if defined(OBE_TEST_HAND_WRITTEN)
using ScenarioEngines = test::EngineTypes;
#else
using ScenarioEngines = ::testing::Types<engine::ReferenceEngineImpl, test::NaiveEngineImpl>;
#endif
TYPED_TEST_SUITE(EngineScenario, ScenarioEngines);

// --- Instruments -------------------------------------------------------------

TYPED_TEST(EngineScenario, OpeningAnInstrumentAnnouncesIt) {
    EXPECT_FALSE(this->engine->listed(kUnopened));
    const Nanos t = this->tick();
    ASSERT_TRUE(this->engine->add_instrument(kUnopened, feed::Symbol::from("NEW"), t));
    EXPECT_TRUE(this->engine->listed(kUnopened));
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{
                                engine::md::directory(kUnopened, feed::Symbol::from("NEW"), t),
                                engine::md::trading(kUnopened, feed::Symbol::from("NEW"), t)}));
    EXPECT_TRUE(this->reports.all.empty());
}

TYPED_TEST(EngineScenario, OpeningAnInstrumentTwiceIsRefusedAndSilent) {
    EXPECT_FALSE(this->engine->add_instrument(kStock, feed::Symbol::from("AGAIN"), this->tick()));
    EXPECT_TRUE(this->md.all.empty());
    // The first symbol stays: it is what a later Add Order carries.
    this->buy(kP, 100);
    ASSERT_EQ(this->md.template of<feed::AddOrder>().size(), 1U);
    EXPECT_EQ(this->md.template of<feed::AddOrder>()[0].stock, kSymbol);
}

TYPED_TEST(EngineScenario, AnUnopenedLocateLooksLikeAnEmptyBook) {
    EXPECT_EQ(this->engine->best(kUnopened, Side::Buy), std::nullopt);
    EXPECT_EQ(this->engine->best(kUnopened, Side::Sell), std::nullopt);
    EXPECT_TRUE(test::levels_of(*this->engine, kUnopened, Side::Buy).empty());
    EXPECT_TRUE(test::orders_of(*this->engine, kUnopened, Side::Sell).empty());
}

// --- Limit orders ------------------------------------------------------------

TYPED_TEST(EngineScenario, LimitOrderWithNothingToTradeAgainstRests) {
    const OrderId id = this->buy(kP, 300);
    EXPECT_EQ(id, 1U);
    EXPECT_EQ(this->reports.all, (std::vector<Report>{Accepted{.order_id = 1,
                                                               .owner = kAnn,
                                                               .token = this->token,
                                                               .locate = kStock,
                                                               .side = Side::Buy,
                                                               .qty = 300,
                                                               .price = kP,
                                                               .kind = OrderKind::Limit,
                                                               .tif = TimeInForce::Day,
                                                               .timestamp = this->now}}));
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{engine::md::add(kStock, this->now, 1, Side::Buy,
                                                                    300, kSymbol, kP)}));
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP, 300}}));
    EXPECT_TRUE(this->asks().empty());
    EXPECT_EQ(this->engine->best(kStock, Side::Buy), (Level{kP, 300}));
    EXPECT_EQ(this->engine->best(kStock, Side::Sell), std::nullopt);
    EXPECT_EQ(this->engine->open_orders(), 1U);
    EXPECT_EQ(this->bid_orders(), (std::vector<RestingOrder>{{1, kAnn, this->token, kP, 300}}));
}

TYPED_TEST(EngineScenario, IdsCountUpFromOneAcrossInstruments) {
    EXPECT_EQ(this->buy(kP, 100), 1U);
    EXPECT_EQ(this->submit(
                  {.owner = kBob, .locate = kOther, .side = Side::Sell, .qty = 100, .price = kP}),
              2U);
    EXPECT_EQ(this->sell(kP + kTick, 100), 3U);
}

TYPED_TEST(EngineScenario, LevelsAreWalkedBestFirstOnBothSides) {
    this->buy(kP - 2 * kTick, 100);
    this->buy(kP, 200);
    this->buy(kP - kTick, 300);
    this->sell(kP + 3 * kTick, 400);
    this->sell(kP + kTick, 500);
    this->sell(kP + 2 * kTick, 600);
    EXPECT_EQ(this->bids(),
              (std::vector<Level>{{kP, 200}, {kP - kTick, 300}, {kP - 2 * kTick, 100}}));
    EXPECT_EQ(this->asks(), (std::vector<Level>{
                                {kP + kTick, 500}, {kP + 2 * kTick, 600}, {kP + 3 * kTick, 400}}));
    EXPECT_EQ(this->engine->best(kStock, Side::Buy), (Level{kP, 200}));
    EXPECT_EQ(this->engine->best(kStock, Side::Sell), (Level{kP + kTick, 500}));
}

TYPED_TEST(EngineScenario, OrdersAtOnePriceShareALevelAndQueueInArrivalOrder) {
    this->buy(kP, 100, kAnn);
    this->buy(kP, 200, kBob);
    this->buy(kP, 300, kCat);
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP, 600}}));
    EXPECT_EQ(this->ids(Side::Buy), (std::vector<OrderId>{1, 2, 3}));
}

TYPED_TEST(EngineScenario, AWalkStopsWhenTheVisitorSaysSo) {
    this->buy(kP, 100);
    this->buy(kP, 100);
    this->buy(kP - kTick, 100);
    int levels = 0;
    this->engine->for_each_level(kStock, Side::Buy, [&levels](const Level&) {
        ++levels;
        return false;
    });
    EXPECT_EQ(levels, 1);
    int orders = 0;
    this->engine->for_each_order(kStock, Side::Buy, [&orders](const RestingOrder&) {
        ++orders;
        return orders < 2;
    });
    EXPECT_EQ(orders, 2);
}

// --- Trading -----------------------------------------------------------------

TYPED_TEST(EngineScenario, CrossingOrderTradesAtTheRestingPrice) {
    this->sell(kP, 300, kBob);
    const engine::Token bob = this->token;
    this->forget();

    // Ann is willing to pay five cents more and does not have to.
    EXPECT_EQ(this->buy(kP + 5 * kTick, 300, kAnn), 2U);
    const engine::Token ann = this->token;

    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{Accepted{.order_id = 2,
                                            .owner = kAnn,
                                            .token = ann,
                                            .locate = kStock,
                                            .side = Side::Buy,
                                            .qty = 300,
                                            .price = kP + 5 * kTick,
                                            .kind = OrderKind::Limit,
                                            .tif = TimeInForce::Day,
                                            .timestamp = this->now},
                                   this->fill(1, kBob, bob, 300, kP, 0, 1, Liquidity::Added),
                                   this->fill(2, kAnn, ann, 300, kP, 0, 1, Liquidity::Removed)}));
    // The market sees the resting order execute and never sees Ann's order.
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{engine::md::execute(kStock, this->now, 1, 300, 1)}));
    EXPECT_TRUE(this->bids().empty());
    EXPECT_TRUE(this->asks().empty());
    EXPECT_EQ(this->engine->open_orders(), 0U);
}

TYPED_TEST(EngineScenario, SellCrossingABidTradesAtTheBid) {
    this->buy(kP, 300, kAnn);
    this->forget();
    this->sell(kP - 5 * kTick, 300, kBob);
    const std::vector<Executed> fills = this->reports.template of<Executed>();
    ASSERT_EQ(fills.size(), 2U);
    EXPECT_EQ(fills[0].price, kP);
    EXPECT_EQ(fills[1].price, kP);
    EXPECT_EQ(this->engine->open_orders(), 0U);
}

TYPED_TEST(EngineScenario, WhatIsLeftOfTheIncomingOrderRestsAndIsPublished) {
    this->sell(kP, 100, kBob);
    this->forget();
    EXPECT_EQ(this->buy(kP, 300, kAnn), 2U);

    const std::vector<Executed> fills = this->reports.template of<Executed>();
    ASSERT_EQ(fills.size(), 2U);
    EXPECT_EQ(fills[0], this->fill(1, kBob, this->token - 1, 100, kP, 0, 1, Liquidity::Added));
    EXPECT_EQ(fills[1], this->fill(2, kAnn, this->token, 100, kP, 200, 1, Liquidity::Removed));
    // Execution first, then the add, and the add shows only what is left.
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{
                  engine::md::execute(kStock, this->now, 1, 100, 1),
                  engine::md::add(kStock, this->now, 2, Side::Buy, 200, kSymbol, kP)}));
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP, 200}}));
    EXPECT_TRUE(this->asks().empty());
}

TYPED_TEST(EngineScenario, RestingOrderPartlyFilledKeepsItsPlace) {
    this->sell(kP, 300, kBob);  // 1
    this->sell(kP, 300, kCat);  // 2
    this->buy(kP, 100, kAnn);   // 3: takes 100 from order 1
    EXPECT_EQ(this->ask_orders(),
              (std::vector<RestingOrder>{{1, kBob, 101, kP, 200}, {2, kCat, 102, kP, 300}}));
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 500}}));

    this->forget();
    this->buy(kP, 250, kAnn);  // 4: the rest of order 1, then 50 of order 2
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{engine::md::execute(kStock, this->now, 1, 200, 2),
                                      engine::md::execute(kStock, this->now, 2, 50, 3)}));
    EXPECT_EQ(this->reports.template of<Executed>(),
              (std::vector<Executed>{this->fill(1, kBob, 101, 200, kP, 0, 2, Liquidity::Added),
                                     this->fill(4, kAnn, 104, 200, kP, 50, 2, Liquidity::Removed),
                                     this->fill(2, kCat, 102, 50, kP, 250, 3, Liquidity::Added),
                                     this->fill(4, kAnn, 104, 50, kP, 0, 3, Liquidity::Removed)}));
    EXPECT_EQ(this->ask_orders(), (std::vector<RestingOrder>{{2, kCat, 102, kP, 250}}));
}

TYPED_TEST(EngineScenario, OlderOrderAtAPriceTradesFirst) {
    this->buy(kP, 100, kAnn);  // 1
    this->buy(kP, 100, kBob);  // 2
    this->buy(kP, 100, kCat);  // 3
    this->forget();
    this->sell(kP, 200, kBob);
    const std::vector<feed::OrderExecuted> trades = this->md.template of<feed::OrderExecuted>();
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0].order_ref, 1U);
    EXPECT_EQ(trades[1].order_ref, 2U);
    EXPECT_EQ(this->ids(Side::Buy), (std::vector<OrderId>{3}));
}

TYPED_TEST(EngineScenario, BetterPriceTradesFirstWhateverItsAge) {
    this->sell(kP + 2 * kTick, 100, kBob);  // 1: oldest, worst price
    this->sell(kP + kTick, 100, kBob);      // 2
    this->sell(kP, 100, kBob);              // 3: newest, best price
    this->forget();

    this->buy(kP + 2 * kTick, 300, kAnn);  // 4: sweeps all three levels
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{engine::md::execute(kStock, this->now, 3, 100, 1),
                                      engine::md::execute(kStock, this->now, 2, 100, 2),
                                      engine::md::execute(kStock, this->now, 1, 100, 3)}));
    // Each trade at the price of the order that was waiting.
    const std::vector<Executed> fills = this->reports.template of<Executed>();
    ASSERT_EQ(fills.size(), 6U);
    EXPECT_EQ(fills[1].price, kP);
    EXPECT_EQ(fills[3].price, kP + kTick);
    EXPECT_EQ(fills[5].price, kP + 2 * kTick);
    EXPECT_EQ(fills[5].leaves, 0U);
    EXPECT_TRUE(this->asks().empty());
    EXPECT_TRUE(this->bids().empty());
}

TYPED_TEST(EngineScenario, HighestBidTradesFirst) {
    this->buy(kP - kTick, 100, kAnn);      // 1
    this->buy(kP, 100, kAnn);              // 2
    this->buy(kP - 2 * kTick, 100, kAnn);  // 3
    this->forget();
    this->sell(kP - 2 * kTick, 250, kBob);
    const std::vector<feed::OrderExecuted> trades = this->md.template of<feed::OrderExecuted>();
    ASSERT_EQ(trades.size(), 3U);
    EXPECT_EQ(trades[0].order_ref, 2U);
    EXPECT_EQ(trades[1].order_ref, 1U);
    EXPECT_EQ(trades[2].order_ref, 3U);
    EXPECT_EQ(trades[2].shares, 50U);
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP - 2 * kTick, 50}}));
}

TYPED_TEST(EngineScenario, LimitOrderStopsAtItsPriceAndTheRestWaitsThere) {
    this->sell(kP, 100);
    this->sell(kP + kTick, 100);
    this->sell(kP + 2 * kTick, 100);
    this->forget();

    this->buy(kP + kTick, 500, kAnn);  // 4
    ASSERT_EQ(this->md.trace(), "EEA");
    EXPECT_EQ(this->md.template of<feed::AddOrder>()[0],
              engine::md::add(kStock, this->now, 4, Side::Buy, 300, kSymbol, kP + kTick));
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP + kTick, 300}}));
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP + 2 * kTick, 100}}));
}

TYPED_TEST(EngineScenario, OrderThatDoesNotReachTheOtherSideDoesNotTrade) {
    this->sell(kP + kTick, 100);
    this->forget();
    this->buy(kP, 100);
    EXPECT_EQ(this->md.trace(), "A");
    EXPECT_TRUE(this->reports.template of<Executed>().empty());
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP, 100}}));
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP + kTick, 100}}));
}

TYPED_TEST(EngineScenario, InstrumentsDoNotTradeWithEachOther) {
    this->submit({.owner = kBob, .locate = kOther, .side = Side::Sell, .qty = 100, .price = kP});
    this->forget();
    this->buy(kP, 100);
    EXPECT_EQ(this->md.trace(), "A");
    EXPECT_EQ(this->engine->open_orders(), 2U);
    EXPECT_EQ(test::levels_of(*this->engine, kOther, Side::Sell), (std::vector<Level>{{kP, 100}}));
}

TYPED_TEST(EngineScenario, AnOwnerCanTradeWithThemselves) {
    this->sell(kP, 100, kAnn);
    this->forget();
    this->buy(kP, 100, kAnn);
    const std::vector<Executed> fills = this->reports.template of<Executed>();
    ASSERT_EQ(fills.size(), 2U);
    EXPECT_EQ(fills[0].owner, kAnn);
    EXPECT_EQ(fills[1].owner, kAnn);
    EXPECT_EQ(this->engine->open_orders(), 0U);
}

TYPED_TEST(EngineScenario, MatchNumbersCountUpFromOneAcrossInstruments) {
    this->sell(kP, 100);
    this->buy(kP, 100);
    this->submit({.owner = kBob, .locate = kOther, .side = Side::Sell, .qty = 100, .price = kP});
    this->submit({.owner = kAnn, .locate = kOther, .side = Side::Buy, .qty = 100, .price = kP});
    const std::vector<feed::OrderExecuted> trades = this->md.template of<feed::OrderExecuted>();
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0].match_number, 1U);
    EXPECT_EQ(trades[0].hdr.locate, kStock);
    EXPECT_EQ(trades[1].match_number, 2U);
    EXPECT_EQ(trades[1].hdr.locate, kOther);
}

TYPED_TEST(EngineScenario, LevelTotalsDoNotOverflowThirtyTwoBits) {
    constexpr Qty kBig = 2'000'000'000;
    this->sell(kP, kBig);
    this->sell(kP, kBig);
    this->sell(kP, kBig);
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, std::uint64_t{3} * kBig}}));
    // A fill-or-kill for the largest order there can be is covered by them.
    this->forget();
    this->buy(kP, 4'000'000'000U, kAnn, TimeInForce::FillOrKill);
    EXPECT_TRUE(this->reports.template of<Cancelled>().empty());
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 2'000'000'000ULL}}));
}

// --- Market orders -----------------------------------------------------------

TYPED_TEST(EngineScenario, MarketOrderTakesWhateverPriceIsThere) {
    this->sell(kP, 100, kBob);
    this->sell(kP + 50 * kTick, 100, kBob);
    this->forget();

    EXPECT_EQ(this->market(Side::Buy, 150), 3U);
    const std::vector<Accepted> accepted = this->reports.template of<Accepted>();
    ASSERT_EQ(accepted.size(), 1U);
    EXPECT_EQ(accepted[0].kind, OrderKind::Market);
    EXPECT_EQ(accepted[0].price, 0U);
    const std::vector<Executed> fills = this->reports.template of<Executed>();
    ASSERT_EQ(fills.size(), 4U);
    EXPECT_EQ(fills[1], this->fill(3, kCat, this->token, 100, kP, 50, 1, Liquidity::Removed));
    EXPECT_EQ(fills[3],
              this->fill(3, kCat, this->token, 50, kP + 50 * kTick, 0, 2, Liquidity::Removed));
    EXPECT_EQ(this->md.trace(), "EE");
    EXPECT_TRUE(this->reports.template of<Cancelled>().empty());
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP + 50 * kTick, 50}}));
}

TYPED_TEST(EngineScenario, MarketSellTakesTheBids) {
    this->buy(kP, 100, kAnn);
    this->buy(kP - 50 * kTick, 100, kAnn);
    this->forget();
    this->market(Side::Sell, 200);
    EXPECT_EQ(this->md.trace(), "EE");
    EXPECT_TRUE(this->bids().empty());
    EXPECT_TRUE(this->asks().empty());
}

TYPED_TEST(EngineScenario, MarketOrderIgnoresItsPriceField) {
    this->sell(kP, 100, kBob);
    this->forget();
    // A price far below the offer: a limit order would not trade.
    this->submit({.owner = kCat,
                  .locate = kStock,
                  .side = Side::Buy,
                  .qty = 100,
                  .price = kTick,
                  .kind = OrderKind::Market});
    EXPECT_EQ(this->md.trace(), "E");
    ASSERT_EQ(this->reports.template of<Accepted>().size(), 1U);
    EXPECT_EQ(this->reports.template of<Accepted>()[0].price, 0U);
}

TYPED_TEST(EngineScenario, MarketOrderWithPriceZeroIsNotRejected) {
    this->sell(kP, 100, kBob);
    this->forget();
    EXPECT_NE(this->market(Side::Buy, 100), 0U);
    EXPECT_TRUE(this->reports.template of<Rejected>().empty());
}

TYPED_TEST(EngineScenario, WhatAMarketOrderCannotFillIsCancelledNotRested) {
    this->sell(kP, 100, kBob);
    this->forget();
    this->market(Side::Buy, 300);
    EXPECT_EQ(this->md.trace(), "E");
    EXPECT_EQ(this->reports.template of<Cancelled>(),
              (std::vector<Cancelled>{{.order_id = 2,
                                       .owner = kCat,
                                       .token = this->token,
                                       .qty = 200,
                                       .leaves = 0,
                                       .reason = CancelReason::NoLiquidity,
                                       .timestamp = this->now}}));
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->engine->open_orders(), 0U);
}

TYPED_TEST(EngineScenario, MarketOrderOnAnEmptyBookIsAcceptedThenCancelled) {
    EXPECT_EQ(this->market(Side::Sell, 100), 1U);
    ASSERT_EQ(this->reports.all.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<Accepted>(this->reports.all[0]));
    ASSERT_TRUE(std::holds_alternative<Cancelled>(this->reports.all[1]));
    EXPECT_EQ(std::get<Cancelled>(this->reports.all[1]).reason, CancelReason::NoLiquidity);
    EXPECT_EQ(std::get<Cancelled>(this->reports.all[1]).qty, 100U);
    EXPECT_TRUE(this->md.all.empty());
}

TYPED_TEST(EngineScenario, MarketOrderNeverRestsWhateverItsTimeInForce) {
    for (const TimeInForce tif :
         {TimeInForce::Day, TimeInForce::ImmediateOrCancel, TimeInForce::FillOrKill}) {
        this->market(Side::Buy, 100, kCat, tif);
    }
    EXPECT_EQ(this->engine->open_orders(), 0U);
    EXPECT_TRUE(this->md.all.empty());
    EXPECT_EQ(this->reports.template of<Cancelled>().size(), 3U);
}

// --- Immediate or cancel -----------------------------------------------------

TYPED_TEST(EngineScenario, IocTakesWhatItCanAndCancelsTheRest) {
    this->sell(kP, 100, kBob);
    this->forget();
    EXPECT_EQ(this->buy(kP, 300, kAnn, TimeInForce::ImmediateOrCancel), 2U);
    EXPECT_EQ(this->md.trace(), "E");
    ASSERT_EQ(this->reports.all.size(), 4U);
    EXPECT_EQ(this->reports.all[3], (Report{Cancelled{.order_id = 2,
                                                      .owner = kAnn,
                                                      .token = this->token,
                                                      .qty = 200,
                                                      .leaves = 0,
                                                      .reason = CancelReason::ImmediateOrCancel,
                                                      .timestamp = this->now}}));
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->engine->open_orders(), 0U);
}

TYPED_TEST(EngineScenario, IocThatFillsCompletelyHasNothingToCancel) {
    this->sell(kP, 300, kBob);
    this->forget();
    this->buy(kP, 300, kAnn, TimeInForce::ImmediateOrCancel);
    EXPECT_TRUE(this->reports.template of<Cancelled>().empty());
    EXPECT_EQ(this->reports.template of<Executed>().size(), 2U);
}

TYPED_TEST(EngineScenario, IocRespectsItsLimit) {
    this->sell(kP + kTick, 100, kBob);
    this->forget();
    this->buy(kP, 100, kAnn, TimeInForce::ImmediateOrCancel);
    EXPECT_TRUE(this->md.all.empty());
    ASSERT_EQ(this->reports.template of<Cancelled>().size(), 1U);
    EXPECT_EQ(this->reports.template of<Cancelled>()[0].qty, 100U);
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP + kTick, 100}}));
    EXPECT_TRUE(this->bids().empty());
}

// --- Fill or kill ------------------------------------------------------------

TYPED_TEST(EngineScenario, FokThatCanFillTradesCompletely) {
    this->sell(kP, 100, kBob);
    this->sell(kP + kTick, 200, kBob);
    this->forget();
    this->buy(kP + kTick, 300, kAnn, TimeInForce::FillOrKill);
    EXPECT_EQ(this->md.trace(), "EE");
    EXPECT_TRUE(this->reports.template of<Cancelled>().empty());
    EXPECT_TRUE(this->asks().empty());
    EXPECT_TRUE(this->bids().empty());
}

TYPED_TEST(EngineScenario, FokThatCannotFillTradesNothingAtAll) {
    this->sell(kP, 100, kBob);
    this->sell(kP + kTick, 100, kBob);
    this->forget();

    EXPECT_EQ(this->buy(kP + kTick, 300, kAnn, TimeInForce::FillOrKill), 3U);
    // Nothing was published: to the market it never happened.
    EXPECT_TRUE(this->md.all.empty());
    ASSERT_EQ(this->reports.all.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<Accepted>(this->reports.all[0]));
    EXPECT_EQ(this->reports.all[1], (Report{Cancelled{.order_id = 3,
                                                      .owner = kAnn,
                                                      .token = this->token,
                                                      .qty = 300,
                                                      .leaves = 0,
                                                      .reason = CancelReason::FillOrKill,
                                                      .timestamp = this->now}}));
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 100}, {kP + kTick, 100}}));
    EXPECT_EQ(this->engine->stats().trades, 0U);
}

TYPED_TEST(EngineScenario, FokCountsOnlySharesAtAcceptablePrices) {
    this->sell(kP, 100, kBob);
    this->sell(kP + 2 * kTick, 500, kBob);  // plenty, but beyond the limit
    this->forget();
    this->buy(kP + kTick, 300, kAnn, TimeInForce::FillOrKill);
    EXPECT_TRUE(this->md.all.empty());
    EXPECT_EQ(this->reports.template of<Cancelled>().size(), 1U);
}

TYPED_TEST(EngineScenario, FokForExactlyWhatIsThereFills) {
    this->sell(kP, 100, kBob);
    this->sell(kP, 150, kCat);
    this->sell(kP + kTick, 50, kBob);
    this->forget();
    this->buy(kP + kTick, 300, kAnn, TimeInForce::FillOrKill);
    EXPECT_EQ(this->md.trace(), "EEE");
    EXPECT_EQ(this->engine->open_orders(), 0U);
}

TYPED_TEST(EngineScenario, FokOneShareShortIsKilled) {
    this->sell(kP, 100, kBob);
    this->sell(kP, 150, kCat);
    this->sell(kP + kTick, 49, kBob);
    this->forget();
    this->buy(kP + kTick, 300, kAnn, TimeInForce::FillOrKill);
    EXPECT_TRUE(this->md.all.empty());
    EXPECT_EQ(this->engine->open_orders(), 3U);
}

TYPED_TEST(EngineScenario, FokSellChecksTheBids) {
    this->buy(kP, 100, kAnn);
    this->buy(kP - kTick, 100, kAnn);
    this->forget();
    this->sell(kP, 200, kBob, TimeInForce::FillOrKill);  // only 100 at 100.00 or better
    EXPECT_TRUE(this->md.all.empty());
    this->sell(kP - kTick, 200, kBob, TimeInForce::FillOrKill);
    EXPECT_EQ(this->md.trace(), "EE");
}

TYPED_TEST(EngineScenario, MarketFokFillsAtAnyPriceOrNotAtAll) {
    this->sell(kP, 100, kBob);
    this->sell(kP + 90 * kTick, 100, kBob);
    this->forget();
    this->market(Side::Buy, 201, kCat, TimeInForce::FillOrKill);
    EXPECT_TRUE(this->md.all.empty());
    ASSERT_EQ(this->reports.template of<Cancelled>().size(), 1U);
    // Killed as a fill-or-kill, not as a market order that ran out.
    EXPECT_EQ(this->reports.template of<Cancelled>()[0].reason, CancelReason::FillOrKill);

    this->forget();
    this->market(Side::Buy, 200, kCat, TimeInForce::FillOrKill);
    EXPECT_EQ(this->md.trace(), "EE");
    EXPECT_TRUE(this->reports.template of<Cancelled>().empty());
}

// --- Cancel ------------------------------------------------------------------

TYPED_TEST(EngineScenario, CancelRemovesTheOrderAndTellsBothAudiences) {
    const OrderId id = this->buy(kP, 300, kAnn);
    const engine::Token ann = this->token;
    this->forget();

    EXPECT_TRUE(this->cancel(kAnn, id));
    EXPECT_EQ(this->reports.all, (std::vector<Report>{Cancelled{.order_id = id,
                                                                .owner = kAnn,
                                                                .token = ann,
                                                                .qty = 300,
                                                                .leaves = 0,
                                                                .reason = CancelReason::Requested,
                                                                .timestamp = this->now}}));
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{engine::md::remove(kStock, this->now, id)}));
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->engine->open_orders(), 0U);
    EXPECT_EQ(this->engine->best(kStock, Side::Buy), std::nullopt);
}

TYPED_TEST(EngineScenario, CancelOfAPartlyFilledOrderReportsWhatWasLeft) {
    const OrderId id = this->sell(kP, 300, kBob);
    this->buy(kP, 100, kAnn);
    this->forget();
    EXPECT_TRUE(this->cancel(kBob, id));
    ASSERT_EQ(this->reports.template of<Cancelled>().size(), 1U);
    EXPECT_EQ(this->reports.template of<Cancelled>()[0].qty, 200U);
    EXPECT_TRUE(this->asks().empty());
}

TYPED_TEST(EngineScenario, CancelFromAnywhereInTheQueueKeepsTheOthersInOrder) {
    for (int i = 0; i < 5; ++i) {
        this->buy(kP, 100, kAnn);  // ids 1 to 5
    }
    EXPECT_TRUE(this->cancel(kAnn, 3));  // the middle
    EXPECT_EQ(this->ids(Side::Buy), (std::vector<OrderId>{1, 2, 4, 5}));
    EXPECT_TRUE(this->cancel(kAnn, 1));  // the front
    EXPECT_EQ(this->ids(Side::Buy), (std::vector<OrderId>{2, 4, 5}));
    EXPECT_TRUE(this->cancel(kAnn, 5));  // the back
    EXPECT_EQ(this->ids(Side::Buy), (std::vector<OrderId>{2, 4}));
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP, 200}}));

    // A later order still goes behind the survivors, and they trade in order.
    this->buy(kP, 100, kAnn);  // 6
    this->forget();
    this->sell(kP, 300, kBob);
    const std::vector<feed::OrderExecuted> trades = this->md.template of<feed::OrderExecuted>();
    ASSERT_EQ(trades.size(), 3U);
    EXPECT_EQ(trades[0].order_ref, 2U);
    EXPECT_EQ(trades[1].order_ref, 4U);
    EXPECT_EQ(trades[2].order_ref, 6U);
}

TYPED_TEST(EngineScenario, CancellingTheLastOrderAtAPriceRemovesTheLevel) {
    this->sell(kP, 100, kBob);          // 1
    this->sell(kP + kTick, 100, kBob);  // 2
    EXPECT_TRUE(this->cancel(kBob, 1));
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP + kTick, 100}}));
    EXPECT_EQ(this->engine->best(kStock, Side::Sell), (Level{kP + kTick, 100}));
}

TYPED_TEST(EngineScenario, APriceCanBeUsedAgainAfterItsLevelEmptied) {
    this->sell(kP, 100, kBob);
    this->buy(kP, 100, kAnn);
    this->sell(kP, 200, kBob);
    this->cancel(kBob, 3);
    this->sell(kP, 300, kBob);
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 300}}));
    EXPECT_EQ(this->ids(Side::Sell), (std::vector<OrderId>{4}));
}

TYPED_TEST(EngineScenario, CancelOfAnUnknownOrderIsRejected) {
    this->buy(kP, 100, kAnn);
    this->forget();
    EXPECT_FALSE(this->cancel(kAnn, 99));
    EXPECT_EQ(this->reports.all, (std::vector<Report>{Rejected{.owner = kAnn,
                                                               .token = 0,
                                                               .order_id = 99,
                                                               .reason = RejectReason::UnknownOrder,
                                                               .timestamp = this->now}}));
    EXPECT_TRUE(this->md.all.empty());
    EXPECT_EQ(this->engine->open_orders(), 1U);
}

TYPED_TEST(EngineScenario, CancelOfSomebodyElsesOrderIsRejectedAndTheOrderStays) {
    const OrderId id = this->buy(kP, 100, kAnn);
    this->forget();
    EXPECT_FALSE(this->cancel(kBob, id));
    EXPECT_EQ(this->reports.all, (std::vector<Report>{Rejected{.owner = kBob,
                                                               .token = 0,
                                                               .order_id = id,
                                                               .reason = RejectReason::NotOwner,
                                                               .timestamp = this->now}}));
    EXPECT_TRUE(this->md.all.empty());
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP, 100}}));
}

TYPED_TEST(EngineScenario, AnOrderCanOnlyBeCancelledOnce) {
    const OrderId id = this->buy(kP, 100, kAnn);
    EXPECT_TRUE(this->cancel(kAnn, id));
    this->forget();
    EXPECT_FALSE(this->cancel(kAnn, id));
    ASSERT_EQ(this->reports.template of<Rejected>().size(), 1U);
    EXPECT_EQ(this->reports.template of<Rejected>()[0].reason, RejectReason::UnknownOrder);
    EXPECT_TRUE(this->md.all.empty());
}

TYPED_TEST(EngineScenario, AnOrderThatFilledCompletelyCannotBeCancelled) {
    const OrderId id = this->sell(kP, 100, kBob);
    this->buy(kP, 100, kAnn);
    this->forget();
    EXPECT_FALSE(this->cancel(kBob, id));
    ASSERT_EQ(this->reports.template of<Rejected>().size(), 1U);
    EXPECT_EQ(this->reports.template of<Rejected>()[0].reason, RejectReason::UnknownOrder);
}

TYPED_TEST(EngineScenario, AnIdThatHasNotBeenIssuedYetIsUnknown) {
    // The boundary of whatever maps ids to orders: one past the newest id, and
    // ids far beyond it.
    const OrderId newest = this->buy(kP, 100, kAnn);
    this->forget();
    for (const OrderId id : {newest + 1, newest + 2, newest + 1000, ~OrderId{0}}) {
        EXPECT_FALSE(this->cancel(kAnn, id)) << "id " << id;
        EXPECT_EQ(this->replace(kAnn, id, 100, kP), 0U) << "id " << id;
    }
    const std::vector<Rejected> rejected = this->reports.template of<Rejected>();
    ASSERT_EQ(rejected.size(), 8U);
    for (const Rejected& r : rejected) {
        EXPECT_EQ(r.reason, RejectReason::UnknownOrder);
    }
    EXPECT_TRUE(this->md.all.empty());
    // The next order gets the id that was just refused, and is then known.
    EXPECT_EQ(this->sell(kP + kTick, 100, kBob), newest + 1);
    EXPECT_TRUE(this->cancel(kBob, newest + 1));
    EXPECT_TRUE(this->cancel(kAnn, newest));
}

TYPED_TEST(EngineScenario, AnOrderThatNeverRestedCannotBeCancelled) {
    const OrderId ioc = this->buy(kP, 100, kAnn, TimeInForce::ImmediateOrCancel);
    const OrderId mkt = this->market(Side::Buy, 100, kAnn);
    EXPECT_FALSE(this->cancel(kAnn, ioc));
    EXPECT_FALSE(this->cancel(kAnn, mkt));
    EXPECT_FALSE(this->cancel(kAnn, 0));
}

// --- Replace -----------------------------------------------------------------

TYPED_TEST(EngineScenario, ReplaceToASmallerSizeKeepsThePlaceAndTheId) {
    this->buy(kP, 300, kAnn);  // 1
    const engine::Token ann = this->token;
    this->buy(kP, 300, kBob);  // 2
    this->forget();

    EXPECT_EQ(this->replace(kAnn, 1, 100, kP), 1U);
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{engine::md::reduce(kStock, this->now, 1, 200)}));
    EXPECT_EQ(this->reports.all, (std::vector<Report>{Replaced{.old_id = 1,
                                                               .new_id = 1,
                                                               .owner = kAnn,
                                                               .token = ann,
                                                               .qty = 100,
                                                               .price = kP,
                                                               .kept_priority = true,
                                                               .timestamp = this->now}}));
    EXPECT_EQ(this->bid_orders(),
              (std::vector<RestingOrder>{{1, kAnn, ann, kP, 100}, {2, kBob, ann + 1, kP, 300}}));
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP, 400}}));

    // Still first in line.
    this->forget();
    this->sell(kP, 50, kCat);
    ASSERT_EQ(this->md.template of<feed::OrderExecuted>().size(), 1U);
    EXPECT_EQ(this->md.template of<feed::OrderExecuted>()[0].order_ref, 1U);
}

TYPED_TEST(EngineScenario, ReplaceToTheSameSizeAndPriceChangesNothing) {
    this->buy(kP, 300, kAnn);
    this->buy(kP, 300, kBob);
    this->forget();
    EXPECT_EQ(this->replace(kAnn, 1, 300, kP), 1U);
    EXPECT_TRUE(this->md.all.empty()) << "nothing the market can see changed";
    ASSERT_EQ(this->reports.template of<Replaced>().size(), 1U);
    EXPECT_TRUE(this->reports.template of<Replaced>()[0].kept_priority);
    EXPECT_EQ(this->ids(Side::Buy), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP, 600}}));
}

TYPED_TEST(EngineScenario, ReplaceToALargerSizeGoesToTheBackUnderANewId) {
    this->buy(kP, 300, kAnn);  // 1
    const engine::Token ann = this->token;
    this->buy(kP, 300, kBob);  // 2
    this->forget();

    EXPECT_EQ(this->replace(kAnn, 1, 500, kP), 3U);
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{engine::md::replace(kStock, this->now, 1, 3, 500, kP)}));
    EXPECT_EQ(this->reports.all, (std::vector<Report>{Replaced{.old_id = 1,
                                                               .new_id = 3,
                                                               .owner = kAnn,
                                                               .token = ann,
                                                               .qty = 500,
                                                               .price = kP,
                                                               .kept_priority = false,
                                                               .timestamp = this->now}}));
    EXPECT_EQ(this->bid_orders(),
              (std::vector<RestingOrder>{{2, kBob, ann + 1, kP, 300}, {3, kAnn, ann, kP, 500}}));
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP, 800}}));
    EXPECT_EQ(this->engine->open_orders(), 2U);
}

TYPED_TEST(EngineScenario, ReplaceToANewPriceJoinsTheBackOfThatLevel) {
    this->buy(kP, 300, kAnn);          // 1
    this->buy(kP + kTick, 100, kBob);  // 2
    this->forget();

    EXPECT_EQ(this->replace(kAnn, 1, 300, kP + kTick), 3U);
    EXPECT_EQ(
        this->md.all,
        (std::vector<MdMessage>{engine::md::replace(kStock, this->now, 1, 3, 300, kP + kTick)}));
    EXPECT_EQ(this->ids(Side::Buy), (std::vector<OrderId>{2, 3}));
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP + kTick, 400}}));
}

TYPED_TEST(EngineScenario, ReplaceToANewPriceWithASmallerSizeStillLosesPriority) {
    this->sell(kP, 300, kBob);  // 1
    this->forget();
    EXPECT_EQ(this->replace(kBob, 1, 100, kP + kTick), 2U);
    EXPECT_EQ(this->md.trace(), "U");
    ASSERT_EQ(this->reports.template of<Replaced>().size(), 1U);
    EXPECT_FALSE(this->reports.template of<Replaced>()[0].kept_priority);
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP + kTick, 100}}));
}

TYPED_TEST(EngineScenario, TheOldIdIsDeadAfterAReplaceThatLostPriority) {
    this->buy(kP, 300, kAnn);
    const OrderId fresh = this->replace(kAnn, 1, 300, kP - kTick);
    ASSERT_EQ(fresh, 2U);
    EXPECT_FALSE(this->cancel(kAnn, 1));
    EXPECT_EQ(this->replace(kAnn, 1, 100, kP), 0U);
    EXPECT_TRUE(this->cancel(kAnn, fresh));
    EXPECT_EQ(this->engine->open_orders(), 0U);
}

TYPED_TEST(EngineScenario, ReplaceThatReachesTheOtherSideTrades) {
    this->sell(kP + kTick, 100, kBob);  // 1
    const engine::Token bob = this->token;
    this->buy(kP, 300, kAnn);  // 2
    const engine::Token ann = this->token;
    this->forget();

    EXPECT_EQ(this->replace(kAnn, 2, 300, kP + kTick), 3U);
    // The market cannot be shown a replace into an order that trades on
    // arrival: the old order is deleted and what survives is added afresh.
    EXPECT_EQ(this->md.all,
              (std::vector<MdMessage>{
                  engine::md::remove(kStock, this->now, 2),
                  engine::md::execute(kStock, this->now, 1, 100, 1),
                  engine::md::add(kStock, this->now, 3, Side::Buy, 200, kSymbol, kP + kTick)}));
    EXPECT_EQ(this->reports.all,
              (std::vector<Report>{
                  Replaced{.old_id = 2,
                           .new_id = 3,
                           .owner = kAnn,
                           .token = ann,
                           .qty = 300,
                           .price = kP + kTick,
                           .kept_priority = false,
                           .timestamp = this->now},
                  this->fill(1, kBob, bob, 100, kP + kTick, 0, 1, Liquidity::Added),
                  this->fill(3, kAnn, ann, 100, kP + kTick, 200, 1, Liquidity::Removed)}));
    EXPECT_EQ(this->bids(), (std::vector<Level>{{kP + kTick, 200}}));
    EXPECT_TRUE(this->asks().empty());
}

TYPED_TEST(EngineScenario, ReplaceThatTradesAwayCompletelyLeavesNothingBehind) {
    this->sell(kP + kTick, 500, kBob);  // 1
    this->buy(kP, 300, kAnn);           // 2
    this->forget();
    EXPECT_EQ(this->replace(kAnn, 2, 300, kP + 2 * kTick), 3U);
    EXPECT_EQ(this->md.trace(), "DE");
    EXPECT_TRUE(this->bids().empty());
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP + kTick, 200}}));
    EXPECT_EQ(this->engine->open_orders(), 1U);
    EXPECT_FALSE(this->cancel(kAnn, 3)) << "the new id filled completely and is gone";
}

TYPED_TEST(EngineScenario, ReplaceOfASellThatReachesTheBidsTrades) {
    this->buy(kP, 100, kAnn);           // 1
    this->sell(kP + kTick, 100, kBob);  // 2
    this->forget();
    EXPECT_EQ(this->replace(kBob, 2, 250, kP), 3U);
    EXPECT_EQ(this->md.trace(), "DEA");
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 150}}));
    EXPECT_TRUE(this->bids().empty());
}

TYPED_TEST(EngineScenario, ReplaceWorksOnWhatIsLeftOfAPartlyFilledOrder) {
    this->sell(kP, 300, kBob);  // 1
    this->buy(kP, 100, kAnn);   // leaves 200
    this->forget();
    EXPECT_EQ(this->replace(kBob, 1, 150, kP), 1U);
    EXPECT_EQ(this->md.all, (std::vector<MdMessage>{engine::md::reduce(kStock, this->now, 1, 50)}));
    EXPECT_EQ(this->asks(), (std::vector<Level>{{kP, 150}}));

    // 250 is smaller than the original 300 but larger than what is open now.
    this->forget();
    EXPECT_EQ(this->replace(kBob, 1, 250, kP), 3U);
    EXPECT_EQ(this->md.trace(), "U");
}

TYPED_TEST(EngineScenario, ReplaceKeepsTheSideOwnerAndToken) {
    this->sell(kP, 300, kBob);
    const engine::Token bob = this->token;
    const OrderId fresh = this->replace(kBob, 1, 400, kP + kTick);
    EXPECT_EQ(this->ask_orders(), (std::vector<RestingOrder>{{fresh, kBob, bob, kP + kTick, 400}}));
    EXPECT_TRUE(this->bids().empty());
}

TYPED_TEST(EngineScenario, ARefusedReplaceChangesNothing) {
    const OrderId id = this->buy(kP, 300, kAnn);
    this->forget();

    EXPECT_EQ(this->replace(kAnn, 99, 100, kP), 0U);
    EXPECT_EQ(this->replace(kBob, id, 100, kP), 0U);
    EXPECT_EQ(this->replace(kAnn, id, 0, kP), 0U);
    EXPECT_EQ(this->replace(kAnn, id, 100, 0), 0U);
    // The first failing check wins.
    EXPECT_EQ(this->replace(kBob, 99, 0, 0), 0U);
    EXPECT_EQ(this->replace(kBob, id, 0, 0), 0U);
    EXPECT_EQ(this->replace(kAnn, id, 0, 0), 0U);

    const std::vector<Rejected> rejected = this->reports.template of<Rejected>();
    ASSERT_EQ(rejected.size(), 7U);
    EXPECT_EQ(this->reports.all.size(), 7U);
    EXPECT_EQ(rejected[0].reason, RejectReason::UnknownOrder);
    EXPECT_EQ(rejected[0].order_id, 99U);
    EXPECT_EQ(rejected[1].reason, RejectReason::NotOwner);
    EXPECT_EQ(rejected[1].owner, kBob);
    EXPECT_EQ(rejected[1].order_id, id);
    EXPECT_EQ(rejected[2].reason, RejectReason::ZeroQuantity);
    EXPECT_EQ(rejected[3].reason, RejectReason::ZeroPrice);
    EXPECT_EQ(rejected[4].reason, RejectReason::UnknownOrder);
    EXPECT_EQ(rejected[5].reason, RejectReason::NotOwner);
    EXPECT_EQ(rejected[6].reason, RejectReason::ZeroQuantity);

    EXPECT_TRUE(this->md.all.empty());
    EXPECT_EQ(this->bid_orders(), (std::vector<RestingOrder>{{id, kAnn, this->token, kP, 300}}));
    // No id was used up either.
    EXPECT_EQ(this->buy(kP, 100, kAnn), id + 1);
}

// --- Rejected new orders -----------------------------------------------------

TYPED_TEST(EngineScenario, InvalidNewOrdersAreRejectedAndUseNoId) {
    EXPECT_EQ(this->buy(kP, 0), 0U);
    EXPECT_EQ(this->buy(0, 100), 0U);
    EXPECT_EQ(this->submit(
                  {.owner = kAnn, .locate = kUnopened, .side = Side::Buy, .qty = 1, .price = kP}),
              0U);
    EXPECT_EQ(
        this->submit({.owner = kAnn, .locate = kStock, .side = kBadSide, .qty = 1, .price = kP}),
        0U);

    const std::vector<Rejected> rejected = this->reports.template of<Rejected>();
    ASSERT_EQ(rejected.size(), 4U);
    EXPECT_EQ(this->reports.all.size(), 4U);
    EXPECT_EQ(rejected[0].reason, RejectReason::ZeroQuantity);
    EXPECT_EQ(rejected[1].reason, RejectReason::ZeroPrice);
    EXPECT_EQ(rejected[2].reason, RejectReason::UnknownInstrument);
    EXPECT_EQ(rejected[3], (Rejected{.owner = kAnn,
                                     .token = this->token,
                                     .order_id = 0,
                                     .reason = RejectReason::BadSide,
                                     .timestamp = this->now}));
    EXPECT_TRUE(this->md.all.empty());
    EXPECT_EQ(this->engine->open_orders(), 0U);
    EXPECT_EQ(this->buy(kP, 100), 1U) << "a rejected order must not use up an id";
}

TYPED_TEST(EngineScenario, TheFirstFailingCheckDecidesTheRejectReason) {
    const auto reason_for = [this](NewOrder order) {
        this->forget();
        EXPECT_EQ(this->submit(order), 0U);
        const std::vector<Rejected> rejected = this->reports.template of<Rejected>();
        return rejected.size() == 1 ? std::optional<RejectReason>(rejected[0].reason)
                                    : std::nullopt;
    };
    EXPECT_EQ(reason_for({.locate = kUnopened, .side = kBadSide, .qty = 0, .price = 0}),
              RejectReason::UnknownInstrument);
    EXPECT_EQ(reason_for({.locate = kStock, .side = kBadSide, .qty = 0, .price = 0}),
              RejectReason::BadSide);
    EXPECT_EQ(reason_for({.locate = kStock, .side = Side::Buy, .qty = 0, .price = 0}),
              RejectReason::ZeroQuantity);
    EXPECT_EQ(
        reason_for({.locate = kStock, .side = Side::Buy, .qty = 0, .kind = OrderKind::Market}),
        RejectReason::ZeroQuantity);
}

// --- Bookkeeping -------------------------------------------------------------

TYPED_TEST(EngineScenario, EveryOutputCarriesTheTimeOfItsRequest) {
    this->sell(kP, 100, kBob);
    this->sell(kP, 100, kBob);
    this->forget();
    this->buy(kP, 300, kAnn);
    const Nanos t = this->now;
    ASSERT_FALSE(this->md.all.empty());
    for (const MdMessage& m : this->md.all) {
        EXPECT_EQ(std::visit([](const auto& msg) { return msg.hdr.timestamp; }, m), t);
        EXPECT_EQ(std::visit([](const auto& msg) { return msg.hdr.locate; }, m), kStock);
    }
    for (const Report& r : this->reports.all) {
        EXPECT_EQ(std::visit([](const auto& rep) { return rep.timestamp; }, r), t);
    }
}

TYPED_TEST(EngineScenario, StatsCountWhatHappened) {
    this->sell(kP, 300, kBob);  // accepted
    this->buy(kP, 100, kAnn);   // accepted, 1 trade of 100
    this->buy(kP + kTick, 500, kAnn,
              TimeInForce::ImmediateOrCancel);            // 1 trade of 200, 300 unfilled
    this->buy(kP, 100, kAnn, TimeInForce::FillOrKill);    // accepted, 100 unfilled
    this->market(Side::Sell, 50);                         // accepted, 50 unfilled
    this->buy(kP, 0);                                     // rejected
    const OrderId id = this->buy(kP - kTick, 100, kAnn);  // accepted
    this->replace(kAnn, id, 50, kP - kTick);              // replace
    this->replace(kAnn, id, 50, 0);                       // rejected
    this->cancel(kBob, id);                               // rejected
    this->cancel(kAnn, id);                               // cancel

    EXPECT_EQ(this->engine->stats(), (engine::EngineStats{.accepted = 6,
                                                          .rejected = 3,
                                                          .cancels = 1,
                                                          .replaces = 1,
                                                          .trades = 2,
                                                          .traded_shares = 300,
                                                          .unfilled_shares = 450}));
}

}  // namespace
