#pragma once

#include <cstdint>

#include "obe/types.hpp"

namespace obe::book {

// What the feed-side book remembers about one resting order.
//
// An Order Executed message carries only a reference number and a share count.
// The price and side needed to update the right level have to come from here,
// which is why the order store is consulted on almost every message.
//
// Note what is absent: no next/prev links, no arrival time. The feed-side book
// never decides who trades, because ITCH names the order that executed. Queue
// position only matters to the matching engine (phase 5).
struct OrderRecord {
    Price price = 0;
    Qty qty = 0;  // shares still resting
    Side side = Side::Buy;

    friend bool operator==(const OrderRecord&, const OrderRecord&) = default;
};

// One price level: the total displayed shares resting at a price.
struct Level {
    Price price = 0;
    std::uint64_t qty = 0;

    friend bool operator==(const Level&, const Level&) = default;
};

// Best bid and offer. A side with no orders has quantity 0 (and price 0).
struct Bbo {
    Price bid_price = 0;
    std::uint64_t bid_qty = 0;
    Price ask_price = 0;
    std::uint64_t ask_qty = 0;

    [[nodiscard]] constexpr bool has_bid() const noexcept { return bid_qty != 0; }
    [[nodiscard]] constexpr bool has_ask() const noexcept { return ask_qty != 0; }
    [[nodiscard]] constexpr bool locked() const noexcept {
        return has_bid() && has_ask() && bid_price == ask_price;
    }
    [[nodiscard]] constexpr bool crossed() const noexcept {
        return has_bid() && has_ask() && bid_price > ask_price;
    }

    friend bool operator==(const Bbo&, const Bbo&) = default;
};

// Published whenever a book's best bid or offer changes in price or size.
struct BboUpdate {
    Locate locate = 0;
    Nanos timestamp = 0;  // of the message that caused the change
    Bbo bbo;

    friend bool operator==(const BboUpdate&, const BboUpdate&) = default;
};

// Invariant counters. On a correct book fed a valid stream every one of these
// stays at zero for the whole day (success criterion 2). A non-zero value is
// never fatal: the message is skipped or clamped, the counter records it, and
// the replay carries on so the full extent of a problem is visible in one run.
struct Counters {
    std::uint64_t unknown_order = 0;    // E, C, X, D or U named an order that is not resting
    std::uint64_t overfill = 0;         // E, C or X took more shares than the order had left
    std::uint64_t duplicate_order = 0;  // A, F or U introduced a reference number already live
    std::uint64_t zero_shares = 0;      // a message with a share count of zero
    std::uint64_t level_mismatch = 0;   // the price levels refused a removal the order store
                                        // said was valid: the two containers disagree

    [[nodiscard]] constexpr bool clean() const noexcept {
        return unknown_order == 0 && overfill == 0 && duplicate_order == 0 && zero_shares == 0 &&
               level_mismatch == 0;
    }

    friend bool operator==(const Counters&, const Counters&) = default;
};

// Statistics, not invariants. Non-zero values are expected on real data.
struct Stats {
    std::uint64_t bbo_updates = 0;
    // A displayed book can be locked (bid == ask) or crossed (bid > ask) while
    // a stock is not in continuous trading. These split such updates by the
    // stock's trading state at the time, so the claim "only outside continuous
    // trading" can be checked against the data. See docs/design.md.
    std::uint64_t locked_while_trading = 0;
    std::uint64_t locked_while_not_trading = 0;
    std::uint64_t crossed_while_trading = 0;
    std::uint64_t crossed_while_not_trading = 0;

    friend bool operator==(const Stats&, const Stats&) = default;
};

// Result of a full walk over every book (BookManager::audit).
struct Audit {
    std::uint64_t levels = 0;        // price levels across all books
    std::uint64_t empty_levels = 0;  // levels with zero quantity: must be 0
    std::uint64_t level_shares = 0;  // sum of every level's quantity
    std::uint64_t open_shares = 0;   // sum of every resting order's shares, tracked
                                     // per message as orders are added and reduced
    std::uint64_t open_orders = 0;   // orders in the store

    // The levels and the per-message bookkeeping must tell the same story.
    [[nodiscard]] constexpr bool clean() const noexcept {
        return empty_levels == 0 && level_shares == open_shares;
    }
};

}  // namespace obe::book
