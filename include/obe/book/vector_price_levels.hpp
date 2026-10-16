#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "obe/book/concepts.hpp"
#include "obe/book/types.hpp"
#include "obe/types.hpp"

// VectorPriceLevels: the optimized side of a book. The levels live in one
// sorted, contiguous array with the best price at the back, instead of in the
// nodes of a std::map. Phase 4, experiment 3.
//
// The hypothesis (spec, phase 4): most activity is near the top of the book,
// so inserts and erases near the end of the array are cheap and stay in cache,
// while the tree pays O(log L) pointer hops to reach the same place.
// `feed_profile <file>` prints how many levels from the best price each update
// lands; docs/optimization-log.md has the profile beside the result.
//
// The contract is PriceLevelsLike (obe/book/concepts.hpp). The judges:
//
//   tests/book/price_levels_test.cpp    the container contract and a model test
//   tests/book/differential_test.cpp    the whole book against the reference
//   scripts/diff_books.sh               the same check on a full real day
//   replay_bench --impl vector-levels   refuses to report timings as valid
//                                       unless the hash matches the reference
//
// DESIGN
//
//  1. The best price at the back.
//     Inserting or erasing at position i of a vector moves everything after
//     i. Updates land near the best price, so with the best at the back they
//     move a handful of elements, and adding a new best level (the most
//     common insert) is a push_back that moves nothing. With the best at the
//     front, the same updates would move the whole side every time.
//
//  2. Both sides with one class: the side is folded into the key.
//     Bids must be stored ascending (best, the highest, at the back) and asks
//     descending (best, the lowest, at the back). Instead of a flag consulted
//     on every comparison, each price is stored as a key: the price itself
//     for bids, and the price with every bit flipped (~price) for asks.
//     Flipping all bits of an unsigned number reverses its order, so for both
//     sides the array is ascending by key and the best level is the largest
//     key. The loops are then identical for both sides and contain no side
//     test at all; the flip is one XOR on the way in and out.
//
//  3. Finding a price: scan the last few, then binary search.
//     A linear scan from the back costs the distance from the best, with one
//     well-predicted branch per step, and that distance is usually 0 to 3.
//     A binary search costs log2(L) steps with unpredictable branches,
//     whatever the distance. So find() scans the last kScan (8) levels and
//     only then falls back to std::lower_bound over the rest: shallow updates
//     never pay for the search, deep ones never pay for a long scan.
//
//  4. The cost being accepted.
//     An insert or erase k levels from the best moves k elements (16 bytes
//     each). Deep updates (feed_profile's "64+" bucket) pay a memmove of the
//     levels above them. On a stock where they are common this design is
//     worse than the tree, and the log says how often that happens on the
//     real day.
//
//  5. Element size: {uint32 key, uint32 padding, uint64 total}, 16 bytes,
//     four per cache line. The total would fit in 32 bits on any real day,
//     but one order may carry up to 2^32 - 1 shares, so a sum of several can
//     overflow 32 bits, and the contract's Level::qty is 64 bits. Packing to 12
//     bytes would need unaligned 64-bit loads for a 25% gain in density:
//     not taken.
//
//  6. Capacity: an empty side owns no memory.
//     Every one of the 65536 books holds two of these, and a day uses fewer
//     than 10,000 securities. A std::vector allocates nothing until the first
//     insert, then grows geometrically; a busy side settles at its high-water
//     mark within the first seconds of trading and never reallocates again.
//     Reserving 64 levels up front would cost 65536 * 2 * 1 KiB = 128 MiB of
//     mostly untouched memory for no measurable gain.
//
//  7. Not done here: an array indexed directly by price in ticks. It needs a
//     known price range and tick size per symbol, and a stock that moves far
//     in a day walks off the end of its array (or makes it enormous). The
//     sorted array needs neither.

namespace obe::book {

class VectorPriceLevels {
 public:
    // An empty side. `side` decides which end is best.
    explicit VectorPriceLevels(Side side) noexcept : flip_(side == Side::Buy ? 0U : ~Price{0}) {}

    // Adds qty shares at price, creating the level if needed. qty > 0.
    void add(Price price, Qty qty) {
        const Price key = price ^ flip_;
        const std::size_t i = lower_bound(key);
        if (i < levels_.size() && levels_[i].key == key) {
            levels_[i].total += qty;
            return;
        }
        levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), Entry{key, qty});
    }

    // Takes qty shares off the level at price and erases the level if that
    // empties it. Returns false, changing nothing, if there is no such level
    // or it holds fewer than qty shares. qty > 0.
    bool remove(Price price, Qty qty) {
        const Price key = price ^ flip_;
        const std::size_t i = lower_bound(key);
        if (i == levels_.size() || levels_[i].key != key || levels_[i].total < qty) {
            return false;
        }
        levels_[i].total -= qty;
        if (levels_[i].total == 0) {
            levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return true;
    }

    // The best level: highest price for Buy, lowest for Sell. nullopt if empty.
    [[nodiscard]] std::optional<Level> best() const noexcept {
        if (levels_.empty()) {
            return std::nullopt;
        }
        return level(levels_.back());
    }

    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

    // Number of price levels.
    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

    // Calls visit(const Level&) from best to worst. Stops when visit returns
    // false.
    template <class Visitor>
    void for_each(Visitor&& visit) const {
        for (std::size_t i = levels_.size(); i > 0; --i) {
            if (!visit(level(levels_[i - 1]))) {
                return;
            }
        }
    }

 private:
    struct Entry {
        Price key;  // price for bids, ~price for asks (design point 2)
        std::uint64_t total;
    };

    // Levels searched one by one from the back before binary search.
    static constexpr std::size_t kScan = 8;

    [[nodiscard]] Level level(const Entry& e) const noexcept {
        return Level{e.key ^ flip_, e.total};
    }

    // The first index whose key is not less than `key` (design point 3).
    [[nodiscard]] std::size_t lower_bound(Price key) const noexcept {
        const std::size_t n = levels_.size();
        const std::size_t stop = n > kScan ? n - kScan : 0;
        std::size_t i = n;
        while (i > stop && levels_[i - 1].key >= key) {
            --i;
        }
        if (i > stop || stop == 0) {
            // Either the scan found the place (a smaller key just below i),
            // or it covered the whole array.
            return i;
        }
        // Every key in the scanned tail is >= key: the place is further down.
        const auto it =
            std::lower_bound(levels_.begin(), levels_.begin() + static_cast<std::ptrdiff_t>(stop),
                             key, [](const Entry& e, Price k) { return e.key < k; });
        return static_cast<std::size_t>(it - levels_.begin());
    }

    std::vector<Entry> levels_;
    Price flip_;
};

static_assert(PriceLevelsLike<VectorPriceLevels>);

}  // namespace obe::book
