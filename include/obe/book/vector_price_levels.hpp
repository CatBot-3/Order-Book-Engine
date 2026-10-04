#pragma once

#include <cstddef>
#include <optional>

#include "obe/book/concepts.hpp"
#include "obe/book/types.hpp"
#include "obe/types.hpp"
#include "obe/util/todo.hpp"

// ============================================================================
//  YOURS TO WRITE: phase 4, experiment 3
// ============================================================================
//
// VectorPriceLevels is the optimized side of a book: the levels live in one
// sorted, contiguous array with the best price at the back, instead of in the
// nodes of a std::map.
//
// The hypothesis (spec, phase 4): most activity is near the top of the book,
// so inserts and erases near the end of the array are cheap and stay in cache,
// while the tree pays O(log L) pointer hops to reach the same place. Before
// writing any code, check the premise: `build/release/apps/feed_profile <file>`
// prints how many levels from the best price each update lands, and how many
// levels a side has when it is touched. Put those numbers in the log entry.
//
// The contract is PriceLevelsLike (obe/book/concepts.hpp). The judges:
//
//   tests/book/price_levels_test.cpp    the container contract and a model test
//   tests/book/differential_test.cpp    the whole book against the reference
//   scripts/diff_books.sh               the same check on a full real day
//   replay_bench --impl vector-levels   refuses to report timings as valid
//                                       unless the hash matches the reference
//
// Every function below throws until you replace its body.
//
// Decisions to make:
//
//  1. Why the best price at the back and not the front?
//     Think about what erase() and insert() on a contiguous array have to move,
//     and where the reference measurements say updates land.
//
//  2. Both sides with one class.
//     "Best at the back" means bids are stored in ascending price order and
//     asks in descending order. The comparison flips with the side. Where do
//     you want that decision made: once per call on a stored flag, or folded
//     into the stored key so the loops are identical? The reference took the
//     flag; you are now free to do better, and to measure whether it matters.
//
//  3. Finding a price: scan or binary search?
//     Binary search is O(log L) with unpredictable branches. A linear scan
//     from the back is O(distance from best) with one very predictable branch,
//     and the profile tells you that distance is usually tiny. Where is the
//     crossover? Is a hybrid worth its complexity? Measure before deciding.
//
//  4. The cost you are accepting.
//     An insert or erase k levels from the best moves k elements. That is the
//     trade: cheap near the top, a memmove for deep updates. feed_profile's
//     "64+" bucket is the tail you are paying for. If it is large for some
//     symbols, say so in the log; that is a real limit of this design.
//
//  5. Element size.
//     Level is {uint32 price, uint64 qty}, which pads to 16 bytes: four per
//     cache line. Is there a tighter layout? Is it worth the conversions?
//
//  6. Capacity.
//     Every one of the 65536 Book objects holds two of these. What should an
//     empty one cost, and when should it first allocate? What happens to the
//     benchmark if every side reserves room for 64 levels up front?
//
//  7. A different design to keep in mind, not for this file: an array indexed
//     directly by price in ticks, with no searching at all. What does it need
//     to know about a symbol that this one does not, and what goes wrong for a
//     stock that moves a long way in a day?

namespace obe::book {

class VectorPriceLevels {
 public:
    // An empty side. `side` decides which end is best.
    explicit VectorPriceLevels(Side side) { static_cast<void>(side); }

    // Adds qty shares at price, creating the level if needed. qty > 0.
    void add(Price price, Qty qty) { util::todo("VectorPriceLevels::add", price, qty); }

    // Takes qty shares off the level at price and erases the level if that
    // empties it. Returns false, changing nothing, if there is no such level
    // or it holds fewer than qty shares. qty > 0.
    bool remove(Price price, Qty qty) { util::todo("VectorPriceLevels::remove", price, qty); }

    // The best level: highest price for Buy, lowest for Sell. nullopt if empty.
    std::optional<Level> best() const { util::todo("VectorPriceLevels::best"); }

    bool empty() const { util::todo("VectorPriceLevels::empty"); }

    // Number of price levels.
    std::size_t size() const { util::todo("VectorPriceLevels::size"); }

    // Calls visit(const Level&) from best to worst. Stops when visit returns
    // false.
    template <class Visitor>
    void for_each(Visitor&& visit) const {
        util::todo("VectorPriceLevels::for_each", visit);
    }

 private:
    // Your storage goes here.
};

static_assert(PriceLevelsLike<VectorPriceLevels>);

}  // namespace obe::book
