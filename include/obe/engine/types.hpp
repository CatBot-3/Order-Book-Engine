#pragma once

#include <cstdint>

#include "obe/types.hpp"

// The matching engine's vocabulary: what a participant can ask for, and what
// the engine tells them back.
//
// Two audiences hear about every change, and they are told different things:
//
//   the owner    gets execution reports (this file). They name the owner, the
//                order and what happened to it. Nobody else sees them.
//   the market   gets market data: anonymous ITCH messages (obe/feed). They
//                describe the displayed book and say nothing about who.
//
// An order that trades the moment it arrives never appears in the market data
// as an order. The market only sees the resting orders it took shares from.

namespace obe::engine {

// Who an order belongs to. The gateway (phase 7) maps a connection to one.
using OwnerId = std::uint32_t;

// The owner's own name for an order. The engine does not interpret it; it is
// copied into every report about the order so the owner can match them up.
using Token = std::uint64_t;

enum class OrderKind : std::uint8_t {
    Limit,   // trade at `price` or better
    Market,  // trade at whatever the other side offers; `price` is ignored
};

enum class TimeInForce : std::uint8_t {
    Day,                // whatever does not trade rests in the book
    ImmediateOrCancel,  // whatever does not trade at once is cancelled
    FillOrKill,         // trades completely at once, or not at all
};

// A request to enter an order.
//
// A market order never rests, whatever its time in force: with no price there
// is no level to put it on. Day and ImmediateOrCancel mean the same for it.
struct NewOrder {
    OwnerId owner = 0;
    Token token = 0;
    Locate locate = 0;
    Side side = Side::Buy;
    Qty qty = 0;
    Price price = 0;
    OrderKind kind = OrderKind::Limit;
    TimeInForce tif = TimeInForce::Day;

    friend bool operator==(const NewOrder&, const NewOrder&) = default;
};

enum class RejectReason : std::uint8_t {
    ZeroQuantity,       // an order or a replace for no shares
    ZeroPrice,          // a limit order or a replace with price 0
    BadSide,            // a side byte that is neither 'B' nor 'S'
    UnknownInstrument,  // no add_instrument() was made for the locate
    UnknownOrder,       // cancel or replace of an order that is not resting
    NotOwner,           // cancel or replace of somebody else's order
};

enum class CancelReason : std::uint8_t {
    Requested,          // the owner cancelled it
    ImmediateOrCancel,  // the part of an immediate-or-cancel order that found nothing
    FillOrKill,         // a fill-or-kill order that could not be filled completely
    NoLiquidity,        // the part of a market order left when the other side ran out
};

// Which role the order played in a trade. The values are the bytes OUCH uses.
enum class Liquidity : char {
    Added = 'A',    // it was resting and somebody else came to it
    Removed = 'R',  // it arrived and took shares from a resting order
};

// --- Reports -----------------------------------------------------------------

// The order passed validation and has an id. Sent before anything else about
// it, including its own executions.
struct Accepted {
    OrderId order_id = 0;
    OwnerId owner = 0;
    Token token = 0;
    Locate locate = 0;
    Side side = Side::Buy;
    Qty qty = 0;
    Price price = 0;  // 0 for a market order
    OrderKind kind = OrderKind::Limit;
    TimeInForce tif = TimeInForce::Day;
    Nanos timestamp = 0;

    friend bool operator==(const Accepted&, const Accepted&) = default;
};

// One trade, as seen by one of the two orders in it. Every trade produces two
// of these with the same match number: one Added, one Removed.
struct Executed {
    OrderId order_id = 0;
    OwnerId owner = 0;
    Token token = 0;
    Qty qty = 0;      // shares in this trade
    Price price = 0;  // always the resting order's price
    Qty leaves = 0;   // shares of this order still open afterwards
    std::uint64_t match_number = 0;
    Liquidity liquidity = Liquidity::Added;
    Nanos timestamp = 0;

    friend bool operator==(const Executed&, const Executed&) = default;
};

// Shares were taken out of the order without trading. `leaves` is always 0
// today: every reason removes all that is left. It is carried so that a later
// partial cancel does not need a new report.
struct Cancelled {
    OrderId order_id = 0;
    OwnerId owner = 0;
    Token token = 0;
    Qty qty = 0;  // shares cancelled
    Qty leaves = 0;
    CancelReason reason = CancelReason::Requested;
    Nanos timestamp = 0;

    friend bool operator==(const Cancelled&, const Cancelled&) = default;
};

// A replace went through. If the order kept its place in the queue it also
// kept its id, and old_id == new_id.
struct Replaced {
    OrderId old_id = 0;
    OrderId new_id = 0;
    OwnerId owner = 0;
    Token token = 0;
    Qty qty = 0;  // open shares after the replace, before any trade it causes
    Price price = 0;
    bool kept_priority = false;
    Nanos timestamp = 0;

    friend bool operator==(const Replaced&, const Replaced&) = default;
};

// The request was refused and changed nothing. `order_id` is the order a
// cancel or replace named, and 0 for a refused new order (it never got one).
// `token` is the new order's token, and 0 for a cancel or replace.
struct Rejected {
    OwnerId owner = 0;
    Token token = 0;
    OrderId order_id = 0;
    RejectReason reason = RejectReason::ZeroQuantity;
    Nanos timestamp = 0;

    friend bool operator==(const Rejected&, const Rejected&) = default;
};

// One resting order, as the engine shows it to a walk over the book.
struct RestingOrder {
    OrderId id = 0;
    OwnerId owner = 0;
    Token token = 0;
    Price price = 0;
    Qty qty = 0;  // shares still open

    friend bool operator==(const RestingOrder&, const RestingOrder&) = default;
};

// Totals since the engine was created.
struct EngineStats {
    std::uint64_t accepted = 0;         // new orders that got an id
    std::uint64_t rejected = 0;         // requests refused, of any kind
    std::uint64_t cancels = 0;          // cancel requests that removed an order
    std::uint64_t replaces = 0;         // replace requests that went through
    std::uint64_t trades = 0;           // matches; each has two sides
    std::uint64_t traded_shares = 0;    // shares over all matches, counted once
    std::uint64_t unfilled_shares = 0;  // cancelled by IOC, FOK and market leftovers

    friend bool operator==(const EngineStats&, const EngineStats&) = default;
};

// The two numbers an engine hands out as it goes. They are not visible in the
// book, and an engine restored from a snapshot must carry on from them: an
// order id or a match number given out twice would name two different things.
struct EngineCounters {
    OrderId last_order_id = 0;            // the id the most recent order got
    std::uint64_t last_match_number = 0;  // the number the most recent trade got

    friend bool operator==(const EngineCounters&, const EngineCounters&) = default;
};

}  // namespace obe::engine
