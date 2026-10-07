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
//     orders only), BadInstruction (see "Instructions"). A rejected order
//     uses up no id.
//     Otherwise the order gets the next id and an Accepted report. Ids
//     increase across all instruments, starting at 1. Then:
//       0. Post-only only: if it would trade, it is Cancelled (reason
//          PostOnly) and nothing else happens.
//       1. FillOrKill only: if the shares it could trade with (see
//          "Instructions" for which those are) do not cover the whole order,
//          it is Cancelled (reason FillOrKill) and nothing else happens.
//       2. It trades while it has shares left and the best resting order on
//          the other side is at an acceptable price. Each trade takes
//          min(shares left, the resting order's DISPLAYED shares), gets the
//          next match number (1, 2, 3, ...) and produces, in this order:
//              market data   'E' naming the resting order
//              report        Executed to the resting order (Added)
//              report        Executed to the incoming order (Removed)
//          A resting order that reaches zero leaves the book. Nothing more is
//          published for it: ITCH sends no delete after a complete fill.
//          (An iceberg shows more instead, and self-match prevention can
//          stop the order here: both under "Instructions".)
//       3. What is left of a Limit Day order rests at the back of its level
//          and is published as 'A' with the shares it displays. What is left
//          of anything else is Cancelled: reason NoLiquidity for a market
//          order, ImmediateOrCancel otherwise.
//     Returns the id.
//
// cancel(owner, id, now) -> bool
//     Rejected, returning false, with UnknownOrder if no such order is
//     resting, or NotOwner if it is somebody else's. Otherwise the order
//     leaves the book: 'D' to the market, Cancelled (reason Requested, with
//     all the shares that were open, displayed or not) to the owner. Returns
//     true.
//
// replace(owner, id, new_qty, new_price, now) -> OrderId
//     `new_qty` is the number of open shares the order should have afterwards,
//     displayed and hidden together.
//     Checks, in order: UnknownOrder, NotOwner, ZeroQuantity, ZeroPrice, and
//     for a post-only order whose new price would trade, WouldTrade. Rejected
//     returns 0 and leaves the order as it was.
//       Same price, new_qty <= open shares: the order keeps its id and its
//         place. The shares come off the hidden part first; 'X' for whatever
//         comes off the displayed part (nothing if none does), then
//         Replaced{old_id == new_id, kept_priority = true}. Returns id.
//       Otherwise: the old order leaves the book and a new Limit Day order
//         with the next id and the same owner, token, side and instructions
//         arrives. Replaced{old_id, new_id, kept_priority = false} is
//         reported first.
//         If the new price would not trade: 'U' and the order rests.
//         If it would: 'D' for the old order, then trades exactly as in
//         submit step 2, then 'A' under the new id for whatever is left.
//         Returns the new id.
//
// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------
//
// A new order can carry three instructions (NewOrder::post_only, display and
// self_match). With none of them set, everything above is the whole story.
//
// Post-only: "rest, or do nothing".
//     Only a Limit Day order can rest, so post_only on anything else is
//     Rejected (BadInstruction). "Would trade" means the best resting order on
//     the other side is at an acceptable price, whoever owns it. The order is
//     Accepted and then Cancelled: it was a valid order that the book had no
//     room for, like a fill-or-kill that is killed.
//     Why it exists: resting orders are often paid a rebate and arriving ones
//     charged a fee. Somebody quoting for the rebate wants to be certain an
//     order never pays the fee because the market moved while it was on the
//     wire.
//
// Iceberg: `display`, when it is above 0 and below `qty`.
//     Only a Limit Day order can rest, so such a display size on anything
//     else is Rejected (BadInstruction). A display of 0, or of qty or more,
//     is simply an order shown in full.
//     On arrival an iceberg trades with its whole size, like any order.
//     When it rests, min(display, shares left) are DISPLAYED and the rest is
//     HIDDEN. The market data, best() and for_each_level() show displayed
//     shares only. Reports to the owner count both: `leaves` is everything
//     still open.
//     An incoming order trades with the displayed shares. When they reach
//     zero and hidden shares remain, the order is REPLENISHED:
//       - min(display, hidden) shares move from hidden to displayed;
//       - the order goes to the BACK of its level's queue: the new slice has
//         no claim on the place the old one earned;
//       - the slice takes the next id as its order reference in the market
//         data and is published as 'A'. The order's own id, the one the owner
//         uses and every report carries, does not change. (So ids are not
//         consecutive once icebergs are about. Observers cannot tell a new
//         slice from a new order, which is the point of hiding the rest.)
//     A trade that empties the displayed shares therefore produces 'E', the
//     two reports, and then 'A' for the new slice. The incoming order goes
//     on trading at that level, and reaches the new slice again after
//     everything that was queued behind the old one.
//     Hidden shares can be traded with: fill-or-kill counts them.
//     cancel and the market data for it use the current reference: 'D' names
//     the slice the market can see.
//     A replace carries the display size over to the new order.
//
// Self-match prevention: `self_match`, when it is not Allow.
//     It applies at one moment only: in step 2, when the best resting order,
//     the one the incoming order would trade with next, has the same owner as
//     the incoming order. Then, instead of a trade:
//       CancelIncoming   What is left of the incoming order is Cancelled
//                        (reason SelfMatch) and it is done: nothing rests,
//                        whatever its time in force.
//       CancelResting    The resting order leaves the book: 'D' to the
//                        market, Cancelled (reason SelfMatch, all its open
//                        shares) to the owner. The incoming order carries on
//                        with whatever is now best.
//       CancelBoth       The resting order leaves the book as above, then
//                        the incoming order stops as in CancelIncoming.
//     Each time, EngineStats::self_matches goes up by one.
//     The instruction stays with an order that rests, and a replace carries
//     it over: it applies again if the replaced order arrives trading.
//     Fill-or-kill has to know before the first trade whether it can be
//     filled, so its check walks the other side as step 2 would:
//       Allow                           every share at an acceptable price
//       CancelIncoming, CancelBoth      the shares ahead of the owner's first
//                                       resting order at an acceptable price
//       CancelResting                   every such share that is not the
//                                       owner's
//     A fill-or-kill that is killed cancels nothing but itself.
//     Any value of `self_match` other than the three named modes means Allow.
//
// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
//
// best(locate, side)            The best level, or nullopt. Displayed shares.
// for_each_level(l, side, f)    f(const book::Level&) from best to worst.
//                               Displayed shares.
// for_each_order(l, side, f)    f(const RestingOrder&) from best price to
//                               worst and, within a price, oldest first: the
//                               order they would trade in. RestingOrder::ref
//                               is 0 unless the order's reference in the
//                               market data differs from its id.
//                               Both visitors return bool; false stops.
// open_orders()                 Orders resting, over all instruments.
// listed(locate)                Whether add_instrument was made for it.
// stats()                       EngineStats.
//
// A locate that was never opened behaves as an empty book in every query.
//
// ---------------------------------------------------------------------------
// Restoring (optional)
// ---------------------------------------------------------------------------
//
// An engine can be recovered after a crash by replaying its journal of
// requests (obe/journal/replay.hpp). That needs nothing beyond the operations
// above. Replaying a long journal takes as long as the trading it records,
// though, so an engine may also let its state be saved and loaded directly: a
// snapshot (obe/journal/snapshot.hpp). An engine that offers the five
// functions below is Restorable. They are not part of EngineLike, and an
// engine without them can still be journaled and replayed.
//
// symbol(locate)                 The symbol the locate was opened with.
// counters()                     The last id and match number given out.
// restore_instrument(l, symbol)  Opens a book and publishes NOTHING. False if
//                                the locate is already open.
// restore_order(l, side, order)  Puts a resting order at the back of its
//                                level, exactly as the RestingOrder describes
//                                it (id, owner, token, displayed and hidden
//                                shares, display size, market reference,
//                                instructions), and publishes and reports
//                                NOTHING. False, changing nothing, if the
//                                instrument is not open, the side is invalid,
//                                the price or the displayed quantity is zero,
//                                or the id is already resting. Orders of one
//                                level must be given oldest first, which is
//                                the order for_each_order visits them in.
// restore_counters(c, stats)     Sets the counters and the statistics.
//
// They are for building an engine up from nothing. Mixing them with trading
// is not supported.

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

// An engine whose state can be saved and loaded without replaying how it got
// there. See "Restoring" above.
template <class E>
concept Restorable =
    EngineLike<E> &&
    requires(E engine, const E const_engine, Locate locate, Side side, const feed::Symbol& symbol,
             const RestingOrder& order, const EngineCounters& counters, const EngineStats& stats) {
        { const_engine.symbol(locate) } -> std::same_as<feed::Symbol>;
        { const_engine.counters() } -> std::same_as<EngineCounters>;
        { engine.restore_instrument(locate, symbol) } -> std::same_as<bool>;
        { engine.restore_order(locate, side, order) } -> std::same_as<bool>;
        engine.restore_counters(counters, stats);
    };

}  // namespace obe::engine
