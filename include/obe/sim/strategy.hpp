#pragma once

#include <cmath>
#include <concepts>
#include <cstdint>

#include "obe/sim/types.hpp"
#include "obe/types.hpp"

// What a quoting strategy is, and the simple ones.
//
// A strategy is asked one question, over and over: given the market as it
// stands and what you hold, what do you want resting on each side? It answers
// with a bid and an ask (either may be absent). It does not send orders or
// cancels. The simulator compares the answer with what is already resting and
// works out the orders: a quote whose price has not changed is left alone, so
// that it keeps its place in the queue, and one whose price has changed is
// cancelled and placed again at the back.
//
// A strategy is consulted when the best bid or offer of the security changes,
// when one of its quotes is filled, and when one of its own changes takes
// effect. Like the feed handler it is resolved at compile time.

namespace obe::sim {

template <class S>
concept Strategy = requires(S& strategy, const MarketView& view) {
    { strategy.quote(view) } -> std::same_as<Quotes>;
};

// The largest price on the tick grid that is not above `price`.
[[nodiscard]] constexpr Price floor_to_tick(Price price, Price tick) noexcept {
    return tick == 0 ? price : price - price % tick;
}

// The smallest price on the tick grid that is not below `price`.
[[nodiscard]] constexpr Price ceil_to_tick(Price price, Price tick) noexcept {
    if (tick == 0 || price % tick == 0) {
        return price;
    }
    return price - price % tick + tick;
}

// Between the doubled integer mid and the dollars a pricing formula works in.
// These are for a strategy's arithmetic only; nothing the simulator counts
// goes through floating point.
[[nodiscard]] constexpr double mid_dollars(std::int64_t mid2) noexcept {
    return static_cast<double>(mid2) / (2.0 * static_cast<double>(kPriceScale));
}

// A dollar amount as a Price, rounded down (for a bid) or up (for an ask) to a
// whole price unit. A result of a formula that should land exactly on a price
// can come out a hair to either side of it; the small tolerance stops that
// from costing a whole unit, and then a whole tick. Amounts below zero give 0.
[[nodiscard]] inline Price floor_price(double dollars) noexcept {
    const double units = std::floor(dollars * static_cast<double>(kPriceScale) + 1e-6);
    return units <= 0.0 ? Price{0} : static_cast<Price>(units);
}
[[nodiscard]] inline Price ceil_price(double dollars) noexcept {
    const double units = std::ceil(dollars * static_cast<double>(kPriceScale) - 1e-6);
    return units <= 0.0 ? Price{0} : static_cast<Price>(units);
}

// The highest price on the grid a bid can have without reaching the real best
// offer, and the lowest an ask can have without reaching the real best bid.
// Quotes are passive ("post-only"): one that would trade at once is refused by
// the simulator, so a strategy whose formula can land anywhere clamps to
// these. Precondition: view.two_sided().
//
// "One tick short of the other side" is the usual way to say it, and is what
// these give when the real prices are on the grid. They need not be: a stock
// under a dollar is quoted in hundredths of a cent, and a strategy quoting it
// in whole cents sees real prices between its ticks. Subtracting a tick from
// such a price gives a limit that is itself off the grid.
[[nodiscard]] constexpr Price passive_bid_limit(const MarketView& view) noexcept {
    return view.bbo.ask_price == 0 ? Price{0} : floor_to_tick(view.bbo.ask_price - 1, view.tick);
}
[[nodiscard]] constexpr Price passive_ask_limit(const MarketView& view) noexcept {
    return ceil_to_tick(view.bbo.bid_price + 1, view.tick);
}

// Never quotes. With it the simulator must be a plain replay: same book, no
// fills, no money. The tests use it to show the simulator leaves the market
// data alone.
struct NoQuotes {
    Quotes quote(const MarketView& /*view*/) noexcept { return {}; }
};

// Joins the real best bid and the real best offer, at the back of each queue.
//
// The simplest market maker there is, and the one that depends most on the
// queue model: it earns the whole spread on every pair of fills, and it is
// filled only when everything that was ahead of it has gone. On real data that
// is exactly when the price is about to move through the level, which is what
// the markout figures are for.
struct JoinBest {
    Qty qty = 100;
    // Stops adding to a position at this size: no bid at or above it, no ask
    // at or below its negative.
    std::int64_t max_inventory = 1'000;

    Quotes quote(const MarketView& view) const noexcept {
        Quotes out;
        if (!view.two_sided()) {
            return out;
        }
        if (view.inventory < max_inventory) {
            out.bid = {view.bbo.bid_price, qty};
        }
        if (view.inventory > -max_inventory) {
            out.ask = {view.bbo.ask_price, qty};
        }
        return out;
    }
};

// Quotes a fixed distance either side of the mid.
//
// The baseline every other strategy is compared with. It knows nothing about
// volatility, time or the risk of holding a position; the only thing it does
// about inventory is stop quoting the side that would add to a position that
// has reached the limit.
struct FixedSpread {
    Price half_spread = 100;  // distance of each quote from the mid, in price units
    Qty qty = 100;
    std::int64_t max_inventory = 1'000;

    Quotes quote(const MarketView& view) const noexcept {
        Quotes out;
        if (!view.two_sided()) {
            return out;
        }
        const std::int64_t mid2 = view.mid2();
        // mid - half and mid + half, from the doubled mid. The bid rounds down
        // to the grid and the ask up, so rounding never narrows the spread.
        const std::int64_t bid2 = mid2 - 2 * static_cast<std::int64_t>(half_spread);
        const std::int64_t ask2 = mid2 + 2 * static_cast<std::int64_t>(half_spread);
        const Price bid =
            bid2 <= 0 ? Price{0} : floor_to_tick(static_cast<Price>(bid2 / 2), view.tick);
        const Price ask = ceil_to_tick(static_cast<Price>((ask2 + 1) / 2), view.tick);
        // Neither can reach the other side of the real book, so no clamp is
        // needed: the bid is rounded down from a price at or below the mid,
        // which is below the real offer, and the ask up from one at or above
        // the mid, which is above the real bid.
        if (view.inventory < max_inventory && bid > 0) {
            out.bid = {bid, qty};
        }
        if (view.inventory > -max_inventory) {
            out.ask = {ask, qty};
        }
        return out;
    }
};

static_assert(Strategy<NoQuotes>);
static_assert(Strategy<JoinBest>);
static_assert(Strategy<FixedSpread>);

}  // namespace obe::sim
