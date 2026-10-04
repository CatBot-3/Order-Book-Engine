#pragma once

#include <concepts>
#include <cstddef>
#include <optional>

#include "obe/book/types.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"

// The matching engine's contract, and the two things it talks to.
//
// There are two engines behind this contract. ReferenceEngine is built from
// standard containers and is the definition of correct. MatchingEngine is the
// one written by hand for speed. Anything here that the reference does, the
// hand-written engine must do identically, down to the order of its output:
// the differential test compares their market data byte for byte and their
// reports one by one.
//
// ---------------------------------------------------------------------------
// The priority rule
// ---------------------------------------------------------------------------
//
// Price first, then time. An incoming order trades with the best-priced
// resting order on the other side; among orders at that price, with the one
// that has been there longest. Every trade happens at the resting order's
// price, so an aggressive order can do better than its limit and never worse.
//
// A replace keeps the order's place in the queue only if it changes nothing
// but the size, and only downwards. A new price, or a larger size, sends the
// order to the back of its (new) level under a new id. Why: letting an order
// grow in place would let its owner reserve a position with one share and
// claim the queue later.
//
// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------
//
// Every operation takes the current time from the caller and stamps all of its
// output with it. The engine never reads a clock, which is what makes a run
// repeatable.
//
// add_instrument(locate, symbol, now) -> bool
//     Opens a book. Publishes a Stock Directory ('R') and a Trading Action
//     ('H', state 'T'). Returns false, publishing nothing, if the locate is
//     already open.
//
// submit(order, now) -> OrderId
//     Checks, in this order, reporting the first that fails as Rejected and
//     returning 0: UnknownInstrument, BadSide, ZeroQuantity, ZeroPrice (limit
//     orders only). A rejected order uses up no id.
//     Otherwise the order gets the next id (1, 2, 3, ... across all
//     instruments) and an Accepted report. Then:
//       1. FillOrKill only: if the shares resting on the other side at
//          acceptable prices do not cover the whole order, it is Cancelled
//          (reason FillOrKill) and nothing else happens.
//       2. It trades while it has shares left and the best resting order on
//          the other side is at an acceptable price. Each trade takes
//          min(shares left, the resting order's shares), gets the next match
//          number (1, 2, 3, ...) and produces, in this order:
//              market data   'E' naming the resting order
//              report        Executed to the resting order (Added)
//              report        Executed to the incoming order (Removed)
//          A resting order that reaches zero leaves the book. Nothing more is
//          published for it: ITCH sends no delete after a complete fill.
//       3. What is left of a Limit Day order rests at the back of its level
//          and is published as 'A' with the shares left. What is left of
//          anything else is Cancelled: reason NoLiquidity for a market order,
//          ImmediateOrCancel otherwise.
//     Returns the id.
//
// cancel(owner, id, now) -> bool
//     Rejected, returning false, with UnknownOrder if no such order is
//     resting, or NotOwner if it is somebody else's. Otherwise the order
//     leaves the book: 'D' to the market, Cancelled (reason Requested, with
//     the shares that were left) to the owner. Returns true.
//
// replace(owner, id, new_qty, new_price, now) -> OrderId
//     `new_qty` is the number of open shares the order should have afterwards.
//     Checks, in order: UnknownOrder, NotOwner, ZeroQuantity, ZeroPrice.
//     Rejected returns 0.
//       Same price, new_qty <= current: the order keeps its id and its place.
//         'X' for the difference (nothing if there is none), then
//         Replaced{old_id == new_id, kept_priority = true}. Returns id.
//       Otherwise: the old order leaves the book and a new Limit Day order
//         with the next id, the same owner, token and side arrives.
//         Replaced{old_id, new_id, kept_priority = false} is reported first.
//         If the new price would not trade: 'U' and the order rests.
//         If it would: 'D' for the old id, then trades exactly as in submit
//         step 2, then 'A' under the new id for whatever is left.
//         Returns the new id.
//
// Self-trading is allowed: an owner's order can trade with another of their
// own. Preventing it is a listed stretch goal.
//
// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
//
// best(locate, side)            The best level, or nullopt.
// for_each_level(l, side, f)    f(const book::Level&) from best to worst.
// for_each_order(l, side, f)    f(const RestingOrder&) from best price to
//                               worst and, within a price, oldest first: the
//                               order they would trade in.
//                               Both visitors return bool; false stops.
// open_orders()                 Orders resting, over all instruments.
// listed(locate)                Whether add_instrument was made for it.
// stats()                       EngineStats.
//
// A locate that was never opened behaves as an empty book in every query.

namespace obe::engine {

// Receives the reports addressed to order owners.
template <class T>
concept ReportSink = requires(T& sink) {
    sink.on_accepted(Accepted{});
    sink.on_executed(Executed{});
    sink.on_cancelled(Cancelled{});
    sink.on_replaced(Replaced{});
    sink.on_rejected(Rejected{});
};

// Receives the market data. These are a subset of the ITCH handler callbacks
// in obe/feed/handler.hpp on purpose: anything that can consume a Nasdaq feed
// can be plugged straight into the engine. A book::BookManager is one such
// thing; the ItchFeedWriter in feed_writer.hpp, which turns the calls into
// bytes, is another.
template <class T>
concept MarketDataSink = requires(T& sink) {
    sink.on_stock_directory(feed::StockDirectory{});
    sink.on_trading_action(feed::TradingAction{});
    sink.on_add(feed::AddOrder{});
    sink.on_execute(feed::OrderExecuted{});
    sink.on_cancel(feed::OrderCancel{});
    sink.on_delete(feed::OrderDelete{});
    sink.on_replace(feed::OrderReplace{});
};

struct NullReports {
    void on_accepted(const Accepted&) noexcept {}
    void on_executed(const Executed&) noexcept {}
    void on_cancelled(const Cancelled&) noexcept {}
    void on_replaced(const Replaced&) noexcept {}
    void on_rejected(const Rejected&) noexcept {}
};

struct NullMarketData {
    void on_stock_directory(const feed::StockDirectory&) noexcept {}
    void on_trading_action(const feed::TradingAction&) noexcept {}
    void on_add(const feed::AddOrder&) noexcept {}
    void on_execute(const feed::OrderExecuted&) noexcept {}
    void on_cancel(const feed::OrderCancel&) noexcept {}
    void on_delete(const feed::OrderDelete&) noexcept {}
    void on_replace(const feed::OrderReplace&) noexcept {}
};

static_assert(ReportSink<NullReports>);
static_assert(MarketDataSink<NullMarketData>);

namespace detail {
// Named visitors for the concept to call the walks with. See the note on
// AnyLevelVisitor in obe/book/concepts.hpp for why these are not lambdas.
struct AnyLevelVisitor {
    bool operator()(const book::Level&) const noexcept { return true; }
};
struct AnyOrderVisitor {
    bool operator()(const RestingOrder&) const noexcept { return true; }
};
}  // namespace detail

template <class E>
concept EngineLike = requires(E engine, const E const_engine, const NewOrder& order, Nanos now,
                              OwnerId owner, OrderId id, Qty qty, Price price, Locate locate,
                              Side side, const feed::Symbol& symbol) {
    { engine.add_instrument(locate, symbol, now) } -> std::same_as<bool>;
    { engine.submit(order, now) } -> std::same_as<OrderId>;
    { engine.cancel(owner, id, now) } -> std::same_as<bool>;
    { engine.replace(owner, id, qty, price, now) } -> std::same_as<OrderId>;
    { const_engine.best(locate, side) } -> std::same_as<std::optional<book::Level>>;
    const_engine.for_each_level(locate, side, detail::AnyLevelVisitor{});
    const_engine.for_each_order(locate, side, detail::AnyOrderVisitor{});
    { const_engine.open_orders() } -> std::convertible_to<std::size_t>;
    { const_engine.listed(locate) } -> std::same_as<bool>;
    { const_engine.stats() } -> std::convertible_to<EngineStats>;
};

}  // namespace obe::engine
