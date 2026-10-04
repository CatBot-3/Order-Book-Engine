#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "obe/book/concepts.hpp"
#include "obe/book/types.hpp"
#include "obe/types.hpp"

// The reference price levels: one side of one book, built on std::map. For
// each price it keeps the total displayed shares resting there.
//
// Its contract is PriceLevelsLike in obe/book/concepts.hpp and its judge is
// tests/book/price_levels_test.cpp.
//
// The decisions in this file:
//
//   Which end is "best". std::map iterates in ascending price order. The best
//   ask is the lowest price (the front) and the best bid is the highest (the
//   back). One class serves both sides and branches on a stored flag to pick
//   the end. The alternatives were a comparator chosen by a template parameter
//   (bids and asks become two different types, and Book and the concept have
//   to know) or negating the key for bids (no branch, but every price that
//   crosses the interface has to be converted back, which is easy to get wrong
//   in exactly one place). The branch is the simplest thing that is obviously
//   right, and "obviously right" is the reference's whole job. Whether the
//   branch costs anything is a question for the optimized version.
//
//   A level that reaches zero is erased. If it were left behind, best() would
//   report a price with no shares, Book::bbo() would publish it as the best
//   bid or offer, and every consumer downstream would see a quote nobody can
//   trade against.
//
//   remove() refuses, and changes nothing, when the level is missing or holds
//   fewer shares than asked. "Take what is there" would keep the replay going
//   with a book that is quietly wrong; refusing turns the same event into a
//   counter that is not zero at the end of the day, which is easy to notice.
//
//   Level quantities are 64-bit. One order is at most 2^32 - 1 shares, but a
//   level is the sum of every order at that price, and a few large orders are
//   enough to pass 32 bits.
//
// Why std::map is slow at this job, which is what phase 4 measures: add and
// remove are O(log L) pointer hops through tree nodes that were allocated one
// at a time, and creating or emptying a level calls the allocator. On a liquid
// stock most updates land within a few levels of the best price, so the tree
// pays its full depth to reach a place a contiguous array would have had in
// cache.

namespace obe::book {

template <class Allocator = std::allocator<std::pair<const Price, std::uint64_t>>>
class BasicPriceLevels {
 public:
    // An empty side. `side` decides which end is best.
    explicit BasicPriceLevels(Side side) noexcept : is_bid_(side == Side::Buy) {}

    // Adds qty shares at price, creating the level if needed. qty > 0.
    void add(Price price, Qty qty) { levels_[price] += qty; }

    // Takes qty shares off the level at price and erases the level if that
    // empties it. Returns false, changing nothing, if there is no such level
    // or it holds fewer than qty shares. qty > 0.
    bool remove(Price price, Qty qty) {
        const auto it = levels_.find(price);
        if (it == levels_.end() || it->second < qty) {
            return false;
        }
        it->second -= qty;
        if (it->second == 0) {
            levels_.erase(it);
        }
        return true;
    }

    // The best level: highest price for Buy, lowest for Sell. nullopt if empty.
    [[nodiscard]] std::optional<Level> best() const {
        if (levels_.empty()) {
            return std::nullopt;
        }
        const auto& [price, qty] = is_bid_ ? *levels_.rbegin() : *levels_.begin();
        return Level{price, qty};
    }

    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

    // Number of price levels.
    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

    // Calls visit(const Level&) from best to worst. Stops when visit returns
    // false. A template, so the visitor is inlined; no std::function.
    template <class Visitor>
    void for_each(Visitor&& visit) const {
        if (is_bid_) {
            // A plain reverse-iterator loop: std::views::reverse would pull in
            // <ranges>, which older Clang releases cannot compile against
            // libstdc++. NOLINTNEXTLINE(modernize-loop-convert)
            for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
                if (!visit(Level{it->first, it->second})) {
                    return;
                }
            }
        } else {
            for (const auto& [price, qty] : levels_) {
                if (!visit(Level{price, qty})) {
                    return;
                }
            }
        }
    }

 private:
    std::map<Price, std::uint64_t, std::less<Price>, Allocator> levels_;
    bool is_bid_;
};

using PriceLevels = BasicPriceLevels<>;

static_assert(PriceLevelsLike<PriceLevels>);

}  // namespace obe::book
