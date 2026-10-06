#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "obe/book/types.hpp"
#include "obe/types.hpp"

// The vocabulary of the market-making simulator.
//
// THIS IS A SIMULATION WITH STATED ASSUMPTIONS, NOT EVIDENCE OF PROFITABILITY.
// It replays a recorded feed and asks what would have happened to quotes that
// were never really in the market. The recorded market did not see them and
// did not react to them. docs/design.md lists every assumption; the ones that
// matter most are in simulator.hpp, next to the code that makes them.
//
// Money is kept in integers, like prices everywhere else in this project.
//
//   prices     Price: ten-thousandths of a dollar, as on the wire
//   cash       price units times shares, signed
//   the mid    kept DOUBLED, as bid + ask. The mid of 10.00 and 10.01 is
//              10.005, which is not a whole price unit; twice it is. Every
//              quantity measured against the mid (spread captured, markout,
//              profit marked to the mid) is therefore kept doubled too, and
//              carries a "2" in its name. Halve it once, when printing.

namespace obe::sim {

inline constexpr Nanos kForever = ~Nanos{0};
inline constexpr Nanos kNanosPerSecond = 1'000'000'000ULL;

// One side of what a strategy wants resting in the book. A quantity of zero
// means "nothing on this side".
struct Quote {
    Price price = 0;
    Qty qty = 0;

    [[nodiscard]] constexpr bool live() const noexcept { return qty != 0; }
    friend bool operator==(const Quote&, const Quote&) = default;
};

struct Quotes {
    Quote bid;
    Quote ask;

    friend bool operator==(const Quotes&, const Quotes&) = default;
};

// What a strategy is shown each time it is asked for quotes.
struct MarketView {
    Nanos now = 0;
    // The real displayed market. The simulator's own quotes are not in it:
    // they were never in the recorded book.
    book::Bbo bbo;
    std::int64_t inventory = 0;  // shares held; negative when short
    Price tick = 100;            // the price grid quotes must lie on
    Nanos end = kForever;        // when quoting stops
    // What is resting for us at this moment. Remaining shares, so a partly
    // filled quote shows less than was asked for.
    Quote working_bid;
    Quote working_ask;

    // Twice the mid price. Only meaningful when both sides are present.
    [[nodiscard]] constexpr std::int64_t mid2() const noexcept {
        return static_cast<std::int64_t>(bbo.bid_price) + static_cast<std::int64_t>(bbo.ask_price);
    }
    [[nodiscard]] constexpr bool two_sided() const noexcept {
        return bbo.has_bid() && bbo.has_ask() && bbo.bid_price < bbo.ask_price;
    }
};

// Why the simulator decided a quote would have traded.
enum class FillReason : std::uint8_t {
    // An order that joined the level after ours was executed. Ours was ahead
    // of it in the queue, so ours would have traded first.
    QueueReached,
    // An order at a worse price than ours was executed. Whoever traded with
    // it would have met our better price first.
    TradedThrough,
    // Our quote was better than every real one on its side, and an order
    // arrived on the other side at a price that reaches it.
    CrossedByAdd,
};

struct Fill {
    Nanos time = 0;
    Side side = Side::Buy;  // Buy: our bid was hit, we bought
    Price price = 0;        // always the quote's own price
    Qty qty = 0;
    std::int64_t mid2 = 0;  // twice the mid just before the event that caused the fill
    FillReason reason = FillReason::QueueReached;

    friend bool operator==(const Fill&, const Fill&) = default;
};

struct SimConfig {
    std::string symbol;  // the one security that is quoted
    Price tick = 100;    // one cent
    // Time between a decision and its effect: a new quote joins the queue this
    // long after the strategy asked for it, and a cancelled quote can still be
    // hit for this long. Zero means the strategy acts on the market it sees,
    // which no real participant can.
    Nanos latency = 0;
    // Quote only inside [start, end), in nanoseconds since midnight like the
    // feed's timestamps. On a real Nasdaq day continuous trading is 09:30 to
    // 16:00; the defaults mean "the whole file".
    Nanos start = 0;
    Nanos end = kForever;
    // Paid to us per share when a resting quote trades (price units). Negative
    // for a fee. Zero by default: a rebate schedule is an input, not something
    // this project knows.
    std::int64_t rebate = 0;
    // How long after each fill the mid is looked at again.
    std::array<Nanos, 2> markout_horizons{kNanosPerSecond, 10 * kNanosPerSecond};
};

// "How much did the price move after each fill", for one horizon.
struct Markout {
    Nanos horizon = 0;
    std::uint64_t shares = 0;  // filled shares old enough to have been measured
    // Sum over those shares of (mid later - fill price) for buys and
    // (fill price - mid later) for sells, doubled. Positive is in our favour.
    // Compare it with the spread captured on the same shares: the difference
    // is how far the price moved against us in that time (adverse selection).
    std::int64_t sum2 = 0;
    std::uint64_t unmeasured_shares = 0;  // filled too close to the end of the run

    friend bool operator==(const Markout&, const Markout&) = default;
};

struct SimReport {
    // --- Trading ---
    std::uint64_t fills = 0;
    std::uint64_t bought = 0;  // shares
    std::uint64_t sold = 0;
    std::uint64_t fills_queue = 0;  // by reason
    std::uint64_t fills_through = 0;
    std::uint64_t fills_crossed = 0;

    // --- Position ---
    std::int64_t inventory = 0;  // at the end
    std::int64_t max_long = 0;
    std::int64_t max_short = 0;  // most negative inventory, as a positive number
    // Time-weighted mean of |inventory| over the quoting window. A statistic,
    // kept in floating point; nothing else here is.
    double mean_abs_inventory = 0;

    // --- Money ---
    std::int64_t cash = 0;     // price units times shares
    std::int64_t rebates = 0;  // already included in cash
    std::int64_t mid2 = 0;     // the last mid seen, doubled
    // Profit with the remaining inventory valued at the last mid, doubled:
    // 2 * cash + inventory * mid2. Always equal to
    // spread2 + inventory_pnl2 + 2 * rebates (see the identity below).
    std::int64_t pnl2 = 0;
    // Sum over fills of shares * (mid - price) for buys and shares *
    // (price - mid) for sells, with the mid taken at the fill, doubled. What
    // quoting away from the mid earned at the moment of each trade.
    std::int64_t spread2 = 0;
    // Sum over every change of the mid of inventory * change, doubled. What
    // holding the position gained or lost afterwards.
    std::int64_t inventory_pnl2 = 0;
    std::int64_t max_drawdown2 = 0;  // largest fall of pnl2 from an earlier high

    // --- Quoting ---
    std::uint64_t placed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t rejected_crossing = 0;    // would have traded at once: quotes are post-only
    std::uint64_t rejected_off_tick = 0;    // price not on the tick grid
    std::uint64_t rejected_self_cross = 0;  // the strategy's bid was not below its ask
    std::uint64_t rejected_closed = 0;      // landed outside the window or while not trading
    std::uint64_t decisions = 0;            // times the strategy was asked

    // --- The market, for scale ---
    std::uint64_t market_executed = 0;  // displayed shares executed in the symbol in the window

    std::array<Markout, 2> markouts{};

    // The identity that ties the three profit figures together. It holds
    // exactly, because all three are integers: a fill changes the profit by
    // exactly its distance from the mid, and a move of the mid changes it by
    // exactly the inventory times the move.
    [[nodiscard]] constexpr bool consistent() const noexcept {
        return pnl2 == 2 * cash + inventory * mid2 &&
               pnl2 == spread2 + inventory_pnl2 + 2 * rebates &&
               inventory == static_cast<std::int64_t>(bought) - static_cast<std::int64_t>(sold);
    }

    friend bool operator==(const SimReport&, const SimReport&) = default;
};

}  // namespace obe::sim
