#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/types.hpp"

// The comparison behind success criterion 7.
//
// The engine knows its own book: it built it from the orders. A BookManager
// fed the engine's market data builds a second book, knowing nothing but the
// messages. If the two show the same shares at the same prices for every
// security, then the market data told the whole truth about the book, and the
// feed handler understood it.

namespace obe::engine {

struct DepthComparison {
    std::uint64_t securities = 0;  // locates with at least one level on either side
    std::uint64_t levels = 0;      // levels the two books agreed on
    std::uint64_t mismatched_sides = 0;
    // The first side that differed, if any.
    Locate bad_locate = 0;
    Side bad_side = Side::Buy;

    [[nodiscard]] constexpr bool ok() const noexcept { return mismatched_sides == 0; }
};

// Compares every level of every book. `manager` is a book::BookManager, or
// anything with the same book(locate).side(side).for_each(f).
//
// `locates` limits the comparison to locates 0 to locates - 1. The default
// covers every locate there can be, including the ones nobody opened: an order
// that turned up in a book that should not exist is a difference too.
template <EngineLike Engine, class Manager>
[[nodiscard]] DepthComparison compare_depth(const Engine& engine, const Manager& manager,
                                            std::size_t locates = std::size_t{1} << 16) {
    DepthComparison out;
    std::vector<book::Level> from_engine;
    std::vector<book::Level> from_feed;
    for (std::size_t i = 0; i < locates; ++i) {
        const auto locate = static_cast<Locate>(i);
        bool any = false;
        for (const Side side : {Side::Buy, Side::Sell}) {
            from_engine.clear();
            from_feed.clear();
            engine.for_each_level(locate, side, [&](const book::Level& level) {
                from_engine.push_back(level);
                return true;
            });
            manager.book(locate).side(side).for_each([&](const book::Level& level) {
                from_feed.push_back(level);
                return true;
            });
            any = any || !from_engine.empty() || !from_feed.empty();
            if (from_engine == from_feed) {
                out.levels += from_engine.size();
            } else {
                if (out.mismatched_sides == 0) {
                    out.bad_locate = locate;
                    out.bad_side = side;
                }
                ++out.mismatched_sides;
            }
        }
        out.securities += any ? 1U : 0U;
    }
    return out;
}

}  // namespace obe::engine
