#pragma once

#include <cstddef>
#include <optional>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"
#include "obe/util/todo.hpp"

// ============================================================================
//  YOURS TO WRITE: phase 5, the matching engine
// ============================================================================
//
// MatchingEngine does what ReferenceEngine does (the contract is in
// concepts.hpp), built the way the spec asks: per side, price levels that each
// hold an intrusive first-in-first-out list of orders taken from a pool, and a
// lookup from order id to the order.
//
// The judges, all labelled needs-your-code until this file is written:
//
//   tests/engine/engine_scenario_test.cpp      every order type and edge case
//   tests/engine/instructions_test.cpp         post-only, icebergs, self-match prevention
//   tests/engine/engine_property_test.cpp      invariants over seeded random flow
//   tests/engine/round_trip_test.cpp           criterion 7: your feed rebuilds your book
//   tests/engine/engine_differential_test.cpp  identical output to the reference
//
// The last one is the strict one. Your market data must equal the reference's
// byte for byte and your reports one by one, so the order in which things are
// published is part of the job. Use the builders in market_data.hpp for the
// messages; they exist so that the two engines cannot differ on a field nobody
// meant to vary.
//
// Try writing the matching loop from the contract before reading the reference
// engine. The reference is a few hundred lines of logic and reading it first makes
// the exercise a transcription.
//
// Decisions to make:
//
//  1. The order node.
//     "Intrusive" means the links live inside the order itself, so putting an
//     order in a queue allocates nothing and taking it out needs only the
//     order. What must a node hold so that cancel(id) can remove it without
//     searching: which neighbours, and how does it reach its level's running
//     total? What must it hold for the reports (owner, token) and for the
//     market data (locate, side, price)? Lay it out and look at sizeof: how
//     many fit in a cache line, and which fields does the matching loop
//     actually touch?
//
//  2. Why a doubly linked list.
//     Matching only ever removes from the front, which a singly linked list
//     does well. Which operation needs the back pointer, and what would it
//     cost without one?
//
//  3. Where orders live.
//     util::Pool<T> (phase 4) gives stable addresses and recycles the slot of
//     the order that just left, which is likely still in cache. The engine is
//     the pool's main customer; if it is not written yet, that comes first.
//
//  4. Finding an order by id.
//     The reference uses std::unordered_map. But the engine assigns the ids
//     itself: 1, 2, 3, ... with no gaps. What is the cheapest possible map
//     from such a key to a pointer? What does that structure cost in memory
//     after a hundred million orders, almost all of them long dead, and what
//     could you do about the dead prefix?
//     Whatever you choose, cancel and replace are handed ids by the outside
//     world: 0, an id not issued yet, the largest 64-bit number. Each must be
//     "unknown order", not an out-of-bounds read.
//
//  5. The levels.
//     A level needs its price, its total, and the two ends of its queue.
//     Keeping std::map<Price, Level> for the first version changes one thing
//     at a time, which makes the comparison with the reference readable.
//     After that, phase 4's observation applies here too: activity
//     concentrates at the top of the book. If levels move to a contiguous
//     array, an order can no longer hold a pointer to its level. What does it
//     hold instead?
//
//  6. Empty levels.
//     The contract's walks never show a level with no orders. Do you erase a
//     level the moment it empties, or keep it around because the price will
//     probably be used again? If you keep it, what must best() and the
//     matching loop skip, and what stops the structure growing all day?
//
//  7. Fill-or-kill.
//     The check has to finish before the first trade, because a trade cannot
//     be taken back once it is published. The level totals make it a walk over
//     levels, not orders. What would it cost without them?
//
//  8. No allocation after warm-up.
//     Count the allocations the reference makes for one resting order: the
//     list node, the index node, and sometimes a tree node. Which of them are
//     left in your design, and when do they happen? Run both engines under
//     `perf stat -e page-faults` or count calls to operator new to check the
//     answer, then put the result in the log.
//
//  9. The instructions (post-only, icebergs, self-match prevention).
//     Read "Instructions" in concepts.hpp, and get the plain engine passing
//     first: an order with no instruction must not pay for them, and
//     tests/engine/instructions_test.cpp will tell you what is left.
//     Things to settle before writing any of it:
//       - An order now has two quantities (shown and hidden), a display size,
//         a second number the market knows it by, and two instructions that
//         stay with it. Which of those does the matching loop touch on every
//         trade, and which only when something unusual happens? Where do the
//         rare ones go so that the common path does not carry them?
//       - A new slice of an iceberg goes to the back of its own level. With
//         an intrusive list that is unlinking the head and linking a tail,
//         with no allocation. What is the case where the order is both head
//         and tail?
//       - The slice takes the next id as its market reference, so the id
//         counter and your id-to-order index move without an order arriving.
//         What does cancel(owner, that_number) have to find there?
//       - Fill-or-kill used to be a walk over level totals. Icebergs make it
//         need a second total per level. Self-match prevention makes it
//         depend on WHOSE orders are in a level and where, which no total
//         can answer. When can you still answer from totals alone, and how
//         do you keep the slow walk off the path of orders that asked for no
//         prevention?
//       - The reference removes a resting order in three places now (cancel,
//         complete fill, self-match). In yours, is that one function?
//
// When this is written, bench/engine_bench compares it with the reference:
//
//   build/release/bench/engine_bench --engine reference
//   build/release/bench/engine_bench --engine pooled

namespace obe::engine {

template <ReportSink Reports, MarketDataSink MarketData>
class MatchingEngine {
 public:
    static constexpr std::size_t kLocates = std::size_t{1} << 16;

    // Sinks are held by reference and must outlive the engine.
    MatchingEngine(Reports& reports, MarketData& market_data)
        : reports_(&reports), md_(&market_data) {}

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // --- Operations (contract: concepts.hpp) ---------------------------------

    bool add_instrument(Locate locate, const feed::Symbol& symbol, Nanos now) {
        util::todo("MatchingEngine::add_instrument", locate, symbol, now, reports_, md_);
    }

    OrderId submit(const NewOrder& order, Nanos now) {
        util::todo("MatchingEngine::submit", order, now);
    }

    bool cancel(OwnerId owner, OrderId id, Nanos now) {
        util::todo("MatchingEngine::cancel", owner, id, now);
    }

    OrderId replace(OwnerId owner, OrderId id, Qty new_qty, Price new_price, Nanos now) {
        util::todo("MatchingEngine::replace", owner, id, new_qty, new_price, now);
    }

    // --- Queries -------------------------------------------------------------

    [[nodiscard]] std::optional<book::Level> best(Locate locate, Side side) const {
        util::todo("MatchingEngine::best", locate, side);
    }

    // f(const book::Level&) -> bool, best to worst; false stops the walk.
    template <class F>
    void for_each_level(Locate locate, Side side, F&& f) const {
        util::todo("MatchingEngine::for_each_level", locate, side, f);
    }

    // f(const RestingOrder&) -> bool, in the order they would trade.
    template <class F>
    void for_each_order(Locate locate, Side side, F&& f) const {
        util::todo("MatchingEngine::for_each_order", locate, side, f);
    }

    [[nodiscard]] std::size_t open_orders() const { util::todo("MatchingEngine::open_orders"); }

    [[nodiscard]] bool listed(Locate locate) const { util::todo("MatchingEngine::listed", locate); }

    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }

 private:
    // Your order node, levels, pool and index go here.

    Reports* reports_;
    MarketData* md_;
    EngineStats stats_{};
};

}  // namespace obe::engine
