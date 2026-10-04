#pragma once

#include <cstdint>
#include <limits>

#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"

// Builders for the ITCH messages an engine publishes, and two small rules both
// engines share.
//
// These are here so that the reference engine and the hand-written one cannot
// disagree about a field nobody meant to vary: the differential test compares
// their feeds byte for byte, and a difference in the round-lot size of a Stock
// Directory would be a failure that teaches nothing.

namespace obe::engine {

namespace md {

[[nodiscard]] constexpr feed::Header header(Locate locate, Nanos now) noexcept {
    return {.locate = locate, .tracking = 0, .timestamp = now};
}

[[nodiscard]] constexpr feed::StockDirectory directory(Locate locate, const feed::Symbol& symbol,
                                                       Nanos now) noexcept {
    feed::StockDirectory m{.hdr = header(locate, now), .stock = symbol};
    m.market_category = 'Q';
    m.financial_status = 'N';
    m.round_lot_size = 100;
    m.round_lots_only = 'N';
    m.issue_classification = 'C';
    m.issue_subtype = {'Z', ' '};
    m.authenticity = 'T';  // 'T' is the specification's code for test data
    m.short_sale_threshold = 'N';
    m.ipo_flag = 'N';
    m.luld_tier = '1';
    m.etp_flag = 'N';
    m.inverse_indicator = 'N';
    return m;
}

[[nodiscard]] constexpr feed::TradingAction trading(Locate locate, const feed::Symbol& symbol,
                                                    Nanos now) noexcept {
    feed::TradingAction m{.hdr = header(locate, now), .stock = symbol};
    m.trading_state = 'T';
    return m;
}

[[nodiscard]] constexpr feed::AddOrder add(Locate locate, Nanos now, OrderId id, Side side,
                                           Qty shares, const feed::Symbol& symbol,
                                           Price price) noexcept {
    return {.hdr = header(locate, now),
            .order_ref = id,
            .side = side,
            .shares = shares,
            .stock = symbol,
            .price = price};
}

[[nodiscard]] constexpr feed::OrderExecuted execute(Locate locate, Nanos now, OrderId resting_id,
                                                    Qty shares, std::uint64_t match) noexcept {
    return {.hdr = header(locate, now),
            .order_ref = resting_id,
            .shares = shares,
            .match_number = match};
}

[[nodiscard]] constexpr feed::OrderCancel reduce(Locate locate, Nanos now, OrderId id,
                                                 Qty shares_removed) noexcept {
    return {.hdr = header(locate, now), .order_ref = id, .shares = shares_removed};
}

[[nodiscard]] constexpr feed::OrderDelete remove(Locate locate, Nanos now, OrderId id) noexcept {
    return {.hdr = header(locate, now), .order_ref = id};
}

[[nodiscard]] constexpr feed::OrderReplace replace(Locate locate, Nanos now, OrderId old_id,
                                                   OrderId new_id, Qty shares,
                                                   Price price) noexcept {
    return {.hdr = header(locate, now),
            .orig_order_ref = old_id,
            .new_order_ref = new_id,
            .shares = shares,
            .price = price};
}

}  // namespace md

[[nodiscard]] constexpr bool valid_side(Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

// The worst price an incoming order will trade at. A market order is a limit
// order with no limit: giving a buy the highest representable price and a sell
// the lowest lets the matching loop treat both kinds the same way.
[[nodiscard]] constexpr Price effective_limit(const NewOrder& order) noexcept {
    if (order.kind == OrderKind::Limit) {
        return order.price;
    }
    return order.side == Side::Buy ? std::numeric_limits<Price>::max() : Price{0};
}

// Whether an incoming order on `side` with limit `limit` may trade with a
// resting order priced at `resting`.
[[nodiscard]] constexpr bool acceptable(Side side, Price limit, Price resting) noexcept {
    return side == Side::Buy ? resting <= limit : resting >= limit;
}

}  // namespace obe::engine
