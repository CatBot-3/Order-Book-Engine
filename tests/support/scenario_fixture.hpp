#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/engines.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"
#include "support/engine_harness.hpp"
#include "support/naive_engine.hpp"

// What the engine's scenario tests share: one instrument, three owners, a
// price to work around, and a fixture that gives every request its own time
// and keeps everything the engine says.
//
// The suites built on it are typed over ScenarioEngines. In the passing build
// that is the reference engine and the test oracle (support/naive_engine.hpp):
// the property tests use the oracle to judge the reference on random flow,
// which is only worth something if the oracle itself agrees with the
// hand-worked cases. In the needs-your-code build it is the hand-written
// engine.

namespace obe::test::scenario {

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

inline constexpr Locate kStock = 7;
inline constexpr Locate kOther = 8;
inline constexpr Locate kUnopened = 9;
inline constexpr OwnerId kAnn = 1;
inline constexpr OwnerId kBob = 2;
inline constexpr OwnerId kCat = 3;
inline constexpr Price kP = 1'000'000;  // $100.00
inline constexpr Price kTick = 100;     // one cent
inline constexpr feed::Symbol kSymbol = feed::Symbol::from("ACME");
// A side byte that is neither 'B' nor 'S', as a corrupt or hostile request
// would carry. The cast is well defined: Side has a fixed underlying type.
// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
inline constexpr Side kBadSide = static_cast<Side>('X');

template <class Impl>
class ScenarioFixture : public ::testing::Test {
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

}  // namespace obe::test::scenario
