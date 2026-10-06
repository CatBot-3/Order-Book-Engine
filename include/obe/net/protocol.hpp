#pragma once

#include <cstddef>
#include <cstdint>

#include "obe/engine/types.hpp"
#include "obe/feed/codec.hpp"
#include "obe/types.hpp"

// The order-entry protocol: what a client sends the gateway over TCP, and what
// the gateway sends back.
//
// It is modelled loosely on Nasdaq's OUCH: a small set of fixed-size binary
// messages, each starting with a one-byte type, integers big-endian, prices
// with four implied decimals. It is not OUCH. The differences that matter:
//
//   - There is no session layer (OUCH rides on SoupBinTCP: logins, heartbeats,
//     sequence numbers, replay after a reconnect). A connection here is the
//     session, and when it closes its orders are cancelled.
//   - A cancel or a replace names the order by the id the exchange assigned
//     (it is in the Accepted message). OUCH names it by the client's token.
//   - A replace's quantity is the open quantity the order should have, as in
//     the engine (docs/design.md, "Decisions that are open to change", item
//     6), not the order's total size.
//
// Framing. A TCP stream has no message boundaries, so the receiver needs to
// know where each message ends. Here the type byte decides it: every type has
// one fixed size, listed below. An unknown type byte cannot be skipped,
// because nothing says how long it is; the only safe answer is to drop the
// connection.
//
// Like the ITCH messages in obe/feed, each struct lists its fields once in
// visit(), and encode, decode and the size all come from that one list.

namespace obe::net {

// ---------------------------------------------------------------------------
// Client to gateway
// ---------------------------------------------------------------------------

// 'O' Enter Order.
struct EnterOrder {
    static constexpr char kType = 'O';
    engine::Token token = 0;
    Locate locate = 0;
    Side side = Side::Buy;
    Qty qty = 0;
    Price price = 0;  // ignored for a market order
    char kind = 'L';  // 'L' limit, 'M' market
    char tif = 'D';   // 'D' day, 'I' immediate-or-cancel, 'F' fill-or-kill

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.token);
        v.u16(m.locate);
        v.side(m.side);
        v.u32(m.qty);
        v.u32(m.price);
        v.ch(m.kind);
        v.ch(m.tif);
    }
    friend bool operator==(const EnterOrder&, const EnterOrder&) = default;
};

// 'U' Replace Order.
struct ReplaceOrder {
    static constexpr char kType = 'U';
    OrderId order_id = 0;
    Qty qty = 0;
    Price price = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.order_id);
        v.u32(m.qty);
        v.u32(m.price);
    }
    friend bool operator==(const ReplaceOrder&, const ReplaceOrder&) = default;
};

// 'X' Cancel Order.
struct CancelOrder {
    static constexpr char kType = 'X';
    OrderId order_id = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.order_id);
    }
    friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

// ---------------------------------------------------------------------------
// Gateway to client
// ---------------------------------------------------------------------------
//
// One message for each report the engine makes (obe/engine/types.hpp). The
// owner is not sent: it is whoever is at the other end of the connection.

// 'A' Order Accepted.
struct OrderAccepted {
    static constexpr char kType = 'A';
    Nanos timestamp = 0;
    engine::Token token = 0;
    OrderId order_id = 0;
    Locate locate = 0;
    Side side = Side::Buy;
    Qty qty = 0;
    Price price = 0;
    char kind = 'L';
    char tif = 'D';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.timestamp);
        v.u64(m.token);
        v.u64(m.order_id);
        v.u16(m.locate);
        v.side(m.side);
        v.u32(m.qty);
        v.u32(m.price);
        v.ch(m.kind);
        v.ch(m.tif);
    }
    friend bool operator==(const OrderAccepted&, const OrderAccepted&) = default;
};

// 'E' Order Executed.
struct OrderExecuted {
    static constexpr char kType = 'E';
    Nanos timestamp = 0;
    engine::Token token = 0;
    OrderId order_id = 0;
    Qty qty = 0;
    Price price = 0;
    Qty leaves = 0;
    std::uint64_t match_number = 0;
    char liquidity = 'A';  // 'A' added, 'R' removed

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.timestamp);
        v.u64(m.token);
        v.u64(m.order_id);
        v.u32(m.qty);
        v.u32(m.price);
        v.u32(m.leaves);
        v.u64(m.match_number);
        v.ch(m.liquidity);
    }
    friend bool operator==(const OrderExecuted&, const OrderExecuted&) = default;
};

// 'C' Order Cancelled.
struct OrderCancelled {
    static constexpr char kType = 'C';
    Nanos timestamp = 0;
    engine::Token token = 0;
    OrderId order_id = 0;
    Qty qty = 0;
    // 'U' the owner asked, 'I' immediate-or-cancel remainder, 'F' fill-or-kill
    // that could not fill, 'L' a market order that ran out of liquidity.
    char reason = 'U';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.timestamp);
        v.u64(m.token);
        v.u64(m.order_id);
        v.u32(m.qty);
        v.ch(m.reason);
    }
    friend bool operator==(const OrderCancelled&, const OrderCancelled&) = default;
};

// 'R' Order Replaced.
struct OrderReplaced {
    static constexpr char kType = 'R';
    Nanos timestamp = 0;
    engine::Token token = 0;
    OrderId old_order_id = 0;
    OrderId new_order_id = 0;  // equal to the old one if the order kept its place
    Qty qty = 0;
    Price price = 0;
    char kept_priority = 'N';  // 'Y' or 'N'

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.timestamp);
        v.u64(m.token);
        v.u64(m.old_order_id);
        v.u64(m.new_order_id);
        v.u32(m.qty);
        v.u32(m.price);
        v.ch(m.kept_priority);
    }
    friend bool operator==(const OrderReplaced&, const OrderReplaced&) = default;
};

// 'J' Rejected.
struct OrderRejected {
    static constexpr char kType = 'J';
    Nanos timestamp = 0;
    engine::Token token = 0;  // of a refused Enter Order; 0 otherwise
    OrderId order_id = 0;     // of a refused Cancel or Replace; 0 otherwise
    // 'Q' zero quantity, 'P' zero price, 'S' bad side, 'I' unknown instrument,
    // 'O' unknown order, 'N' not the owner, 'F' a field the gateway could not
    // interpret (an order kind or time in force that does not exist).
    char reason = 'Q';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.timestamp);
        v.u64(m.token);
        v.u64(m.order_id);
        v.ch(m.reason);
    }
    friend bool operator==(const OrderRejected&, const OrderRejected&) = default;
};

// ---------------------------------------------------------------------------
// Sizes and framing
// ---------------------------------------------------------------------------

inline constexpr std::size_t kEnterOrderSize = feed::kWireSize<EnterOrder>;
inline constexpr std::size_t kReplaceOrderSize = feed::kWireSize<ReplaceOrder>;
inline constexpr std::size_t kCancelOrderSize = feed::kWireSize<CancelOrder>;

// Pinned here so that a change to a field list is a compile error and not a
// silent change of the wire format.
static_assert(kEnterOrderSize == 22);
static_assert(kReplaceOrderSize == 17);
static_assert(kCancelOrderSize == 9);
static_assert(feed::kWireSize<OrderAccepted> == 38);
static_assert(feed::kWireSize<OrderExecuted> == 46);
static_assert(feed::kWireSize<OrderCancelled> == 30);
static_assert(feed::kWireSize<OrderReplaced> == 42);
static_assert(feed::kWireSize<OrderRejected> == 26);

inline constexpr std::size_t kMaxRequestSize = 22;
inline constexpr std::size_t kMaxResponseSize = 46;

// The size of a client-to-gateway message with this type byte, or 0 if no such
// message exists.
[[nodiscard]] constexpr std::size_t request_size(char type) noexcept {
    switch (type) {
        case EnterOrder::kType:
            return kEnterOrderSize;
        case ReplaceOrder::kType:
            return kReplaceOrderSize;
        case CancelOrder::kType:
            return kCancelOrderSize;
        default:
            return 0;
    }
}

// The size of a gateway-to-client message with this type byte, or 0.
[[nodiscard]] constexpr std::size_t response_size(char type) noexcept {
    switch (type) {
        case OrderAccepted::kType:
            return feed::kWireSize<OrderAccepted>;
        case OrderExecuted::kType:
            return feed::kWireSize<OrderExecuted>;
        case OrderCancelled::kType:
            return feed::kWireSize<OrderCancelled>;
        case OrderReplaced::kType:
            return feed::kWireSize<OrderReplaced>;
        case OrderRejected::kType:
            return feed::kWireSize<OrderRejected>;
        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// Between the wire and the engine's own types
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr char to_wire(engine::OrderKind kind) noexcept {
    return kind == engine::OrderKind::Market ? 'M' : 'L';
}

[[nodiscard]] constexpr char to_wire(engine::TimeInForce tif) noexcept {
    switch (tif) {
        case engine::TimeInForce::ImmediateOrCancel:
            return 'I';
        case engine::TimeInForce::FillOrKill:
            return 'F';
        case engine::TimeInForce::Day:
            break;
    }
    return 'D';
}

[[nodiscard]] constexpr char to_wire(engine::CancelReason reason) noexcept {
    switch (reason) {
        case engine::CancelReason::ImmediateOrCancel:
            return 'I';
        case engine::CancelReason::FillOrKill:
            return 'F';
        case engine::CancelReason::NoLiquidity:
            return 'L';
        case engine::CancelReason::Requested:
            break;
    }
    return 'U';
}

inline constexpr char kRejectBadField = 'F';

[[nodiscard]] constexpr char to_wire(engine::RejectReason reason) noexcept {
    switch (reason) {
        case engine::RejectReason::ZeroQuantity:
            return 'Q';
        case engine::RejectReason::ZeroPrice:
            return 'P';
        case engine::RejectReason::BadSide:
            return 'S';
        case engine::RejectReason::UnknownInstrument:
            return 'I';
        case engine::RejectReason::UnknownOrder:
            return 'O';
        case engine::RejectReason::NotOwner:
            return 'N';
    }
    return kRejectBadField;
}

// Turns an Enter Order into the engine's request. Returns false, leaving out
// alone, if the kind or the time in force is not one of the defined bytes: the
// engine has no way to represent them, so the gateway must refuse the order
// itself. Every other field is passed through for the engine to judge.
[[nodiscard]] constexpr bool to_engine(const EnterOrder& m, engine::OwnerId owner,
                                       engine::NewOrder& out) noexcept {
    engine::NewOrder order{.owner = owner,
                           .token = m.token,
                           .locate = m.locate,
                           .side = m.side,
                           .qty = m.qty,
                           .price = m.price};
    switch (m.kind) {
        case 'L':
            order.kind = engine::OrderKind::Limit;
            break;
        case 'M':
            order.kind = engine::OrderKind::Market;
            break;
        default:
            return false;
    }
    switch (m.tif) {
        case 'D':
            order.tif = engine::TimeInForce::Day;
            break;
        case 'I':
            order.tif = engine::TimeInForce::ImmediateOrCancel;
            break;
        case 'F':
            order.tif = engine::TimeInForce::FillOrKill;
            break;
        default:
            return false;
    }
    out = order;
    return true;
}

[[nodiscard]] constexpr OrderAccepted to_wire(const engine::Accepted& r) noexcept {
    return {.timestamp = r.timestamp,
            .token = r.token,
            .order_id = r.order_id,
            .locate = r.locate,
            .side = r.side,
            .qty = r.qty,
            .price = r.price,
            .kind = to_wire(r.kind),
            .tif = to_wire(r.tif)};
}

[[nodiscard]] constexpr OrderExecuted to_wire(const engine::Executed& r) noexcept {
    return {.timestamp = r.timestamp,
            .token = r.token,
            .order_id = r.order_id,
            .qty = r.qty,
            .price = r.price,
            .leaves = r.leaves,
            .match_number = r.match_number,
            .liquidity = static_cast<char>(r.liquidity)};
}

[[nodiscard]] constexpr OrderCancelled to_wire(const engine::Cancelled& r) noexcept {
    return {.timestamp = r.timestamp,
            .token = r.token,
            .order_id = r.order_id,
            .qty = r.qty,
            .reason = to_wire(r.reason)};
}

[[nodiscard]] constexpr OrderReplaced to_wire(const engine::Replaced& r) noexcept {
    return {.timestamp = r.timestamp,
            .token = r.token,
            .old_order_id = r.old_id,
            .new_order_id = r.new_id,
            .qty = r.qty,
            .price = r.price,
            .kept_priority = r.kept_priority ? 'Y' : 'N'};
}

[[nodiscard]] constexpr OrderRejected to_wire(const engine::Rejected& r) noexcept {
    return {.timestamp = r.timestamp,
            .token = r.token,
            .order_id = r.order_id,
            .reason = to_wire(r.reason)};
}

}  // namespace obe::net
