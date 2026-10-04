#pragma once

#include <cstddef>
#include <optional>

#include "obe/book/concepts.hpp"
#include "obe/book/types.hpp"
#include "obe/types.hpp"
#include "obe/util/todo.hpp"

// ============================================================================
//  YOURS TO WRITE (spec section 10: the price levels are written by hand)
// ============================================================================
//
// PriceLevels is one side of one book in the reference implementation of
// phase 2: for each price, the total displayed shares resting there. The spec
// fixes the container for the reference version, std::map.
//
// The interface and its contract are in obe/book/concepts.hpp
// (PriceLevelsLike). The judge is tests/book/price_levels_test.cpp. Every
// function below currently throws; replace each util::todo(...) with the real
// body and add whatever private members you need.
//
// Things to settle before writing it:
//
//  1. Which end is "best"?
//     std::map iterates in ascending key order. The best ask is the lowest
//     price and the best bid is the highest, and this one class serves both
//     sides. There are at least three ways to arrange that:
//       (a) a comparator chosen by a template parameter, so bids and asks are
//           two different types;
//       (b) one map type, and a branch on the side that picks the front or the
//           back;
//       (c) one map type whose key is transformed for one side so that
//           ascending order is already best-first.
//     Which costs a branch per call? Which duplicates code or changes the
//     concept? Which makes for_each awkward? Pick one and write the reason in
//     docs/design.md. The reason is worth more than the choice.
//
//  2. A level whose quantity reaches zero must be erased, not left at zero.
//     Trace what Book::bbo() would report for a side whose best level is an
//     empty husk. (This is the "empty level left behind" counter in the spec.)
//
//  3. remove() on a missing level, or for more shares than the level holds,
//     returns false and changes nothing. Why "changes nothing" and not "take
//     what is there"? Think about which is easier to notice during a
//     ten-gigabyte replay: a counter that is not zero, or a book that is
//     quietly a little wrong.
//
//  4. Level quantities are 64-bit while order sizes are 32-bit. Why is the
//     wider type needed on one and not the other?
//
//  5. What is the cost of add() and remove() in terms of the number of levels,
//     and where in the book do most updates land on a liquid stock? Hold on to
//     your answer: it is the hypothesis behind phase 4 experiment 3, and the
//     cache-miss counter is how you will test it.
//
// for_each is a template so the visitor is inlined; there is no std::function
// here. It must stop as soon as the visitor returns false (book_view asks for
// the top N levels and should not pay for the rest).

namespace obe::book {

class PriceLevels {
 public:
    // An empty side. Remember which side this is: it decides what "best" means.
    explicit PriceLevels(Side side) { static_cast<void>(side); }

    // Adds qty shares at price, creating the level if needed. qty > 0.
    void add(Price price, Qty qty) { util::todo("PriceLevels::add", price, qty); }

    // Takes qty shares off the level at price and erases the level if that
    // empties it. Returns false, changing nothing, if there is no such level
    // or it holds fewer than qty shares. qty > 0.
    bool remove(Price price, Qty qty) { util::todo("PriceLevels::remove", price, qty); }

    // The best level: highest price for Buy, lowest for Sell. nullopt if empty.
    std::optional<Level> best() const { util::todo("PriceLevels::best"); }

    bool empty() const { util::todo("PriceLevels::empty"); }

    // Number of price levels.
    std::size_t size() const { util::todo("PriceLevels::size"); }

    // Calls visit(const Level&) from best to worst. Stops when visit returns
    // false.
    template <class Visitor>
    void for_each(Visitor&& visit) const {
        util::todo("PriceLevels::for_each", visit);
    }

 private:
    // Your storage goes here.
};

static_assert(PriceLevelsLike<PriceLevels>);

}  // namespace obe::book
