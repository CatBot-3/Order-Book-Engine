#pragma once

#include <cstdint>
#include <optional>

#include "obe/book/concepts.hpp"
#include "obe/book/types.hpp"
#include "obe/types.hpp"

namespace obe::book {

// The displayed book of one security: a bid side and an ask side.
//
// A Book holds no orders. Orders live in the order store, which is shared by
// every book, because reference numbers are unique across the whole feed and a
// lookup should not have to find the book first. BookManager owns both and is
// the only thing that changes a Book.
template <PriceLevelsLike Levels>
class Book {
 public:
    Book() : bids_(Side::Buy), asks_(Side::Sell) {}

    [[nodiscard]] const Levels& bids() const noexcept { return bids_; }
    [[nodiscard]] const Levels& asks() const noexcept { return asks_; }

    // Any side byte other than 'B' is treated as a sell. The feed layer does
    // not validate field values; see the note on Side in obe/types.hpp.
    [[nodiscard]] Levels& side(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
    [[nodiscard]] const Levels& side(Side s) const noexcept {
        return s == Side::Buy ? bids_ : asks_;
    }

    // Best bid and offer, read from the levels.
    [[nodiscard]] Bbo bbo() const {
        Bbo out;
        if (const std::optional<Level> bid = bids_.best()) {
            out.bid_price = bid->price;
            out.bid_qty = bid->qty;
        }
        if (const std::optional<Level> ask = asks_.best()) {
            out.ask_price = ask->price;
            out.ask_qty = ask->qty;
        }
        return out;
    }

    // Shares executed against displayed orders: 'E' messages and printable 'C'
    // messages. 'P' and 'Q' prints are not included; they never touch the
    // displayed book.
    [[nodiscard]] std::uint64_t executed_shares() const noexcept { return executed_shares_; }

    // The Stock Trading Action state last seen for this security: 'T' trading,
    // 'H' halted, 'P' paused, 'Q' quotation only. A space until one arrives.
    [[nodiscard]] char trading_state() const noexcept { return trading_state_; }

 private:
    template <OrderStoreLike, PriceLevelsLike, BboListener>
    friend class BookManager;

    Levels bids_;
    Levels asks_;
    Bbo last_published_{};
    std::uint64_t executed_shares_ = 0;
    char trading_state_ = ' ';
};

}  // namespace obe::book
