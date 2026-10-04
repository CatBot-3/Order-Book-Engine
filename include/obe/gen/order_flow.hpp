#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/gen/rng.hpp"
#include "obe/types.hpp"

// A seeded stream of order-entry requests for the matching engine.
//
// Where make_synthetic_feed() writes ITCH messages directly and decides by
// dice which order executes, this generator only asks: it produces new orders,
// cancels and replaces, and the engine decides what trades. The market data
// that comes out is therefore a book that obeys price-time priority and is
// never crossed, which makes it a far better stand-in for a real feed. It is
// what CI fixtures and benchmark workloads are made from, so that no Nasdaq
// data has to be redistributed.
//
// It is still not a market. Each symbol has a mid price that takes a random
// walk, most orders are placed a few ticks behind it, and a minority reach
// across it. There are no participants with intentions, no correlation between
// symbols and no reaction to what just traded. Numbers measured on this flow
// describe the code, not Nasdaq.
//
// The generator needs to know which orders are resting, to have something to
// cancel and replace. It learns that the way a participant would: it is a
// report sink (engine::ReportSink), and whoever runs the engine routes the
// reports to it.
//
// The same config gives the same requests everywhere: integer arithmetic only,
// the project's own PRNG, and no iteration over a hash table.

namespace obe::gen {

struct FlowConfig {
    std::uint64_t seed = 1;
    std::uint32_t symbols = 16;  // 1 to 60000; they get locates 1 to `symbols`
    std::uint32_t owners = 8;    // at least 1; they get owner ids 1 to `owners`
    std::uint32_t target_live_orders = 2'000;
    // Requests that are wrong on purpose (unknown order, somebody else's
    // order, zero shares and so on), per million. They exercise the rejects.
    std::uint32_t bad_per_million = 500;
};

enum class CommandKind : std::uint8_t { New, Cancel, Replace };

// One request, with the time it is made at.
struct Command {
    CommandKind kind = CommandKind::New;
    Nanos now = 0;
    engine::NewOrder order{};   // New
    engine::OwnerId owner = 0;  // Cancel, Replace: who is asking
    OrderId target = 0;         // Cancel, Replace: the order
    Qty qty = 0;                // Replace: the new open quantity
    Price price = 0;            // Replace: the new price

    friend bool operator==(const Command&, const Command&) = default;
};

// Hands a command to an engine.
template <class Engine>
void apply(Engine& engine, const Command& c) {
    switch (c.kind) {
        case CommandKind::New:
            engine.submit(c.order, c.now);
            break;
        case CommandKind::Cancel:
            engine.cancel(c.owner, c.target, c.now);
            break;
        case CommandKind::Replace:
            engine.replace(c.owner, c.target, c.qty, c.price, c.now);
            break;
    }
}

// The symbol the generators give a locate: "S00001", "S00002", ...
[[nodiscard]] constexpr feed::Symbol flow_symbol(Locate locate) noexcept {
    feed::Symbol s;
    s.raw[0] = 'S';
    unsigned value = locate;
    for (std::size_t i = 5; i >= 1; --i) {
        s.raw[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    return s;
}

class OrderFlow {
 public:
    static constexpr Price kTick = 100;  // one cent

    explicit OrderFlow(const FlowConfig& cfg) : cfg_(cfg), rng_(cfg.seed) {
        cfg_.symbols = std::clamp<std::uint32_t>(cfg_.symbols, 1, 60'000);
        cfg_.owners = std::max<std::uint32_t>(cfg_.owners, 1);
        cfg_.target_live_orders = std::max<std::uint32_t>(cfg_.target_live_orders, 1);
        mids_.resize(cfg_.symbols + 1U);
        for (std::uint32_t i = 1; i <= cfg_.symbols; ++i) {
            mids_[i] = static_cast<std::uint32_t>(rng_.between(500, 50'000));  // $5 to $500
        }
    }

    // Opens every symbol on the engine. Call once, before the first next().
    template <class Engine>
    void open(Engine& engine) {
        for (std::uint32_t i = 1; i <= cfg_.symbols; ++i) {
            const auto locate = static_cast<Locate>(i);
            engine.add_instrument(locate, flow_symbol(locate), tick_clock());
        }
    }

    // The next request. Apply it to the engine before asking for another: the
    // reports it causes are how the generator learns what is resting.
    [[nodiscard]] Command next() {
        const Nanos now = tick_clock();
        Command c = choose();
        c.now = now;
        return c;
    }

    // --- engine::ReportSink --------------------------------------------------

    void on_accepted(const engine::Accepted& r) {
        insert(Live{r.order_id, r.owner, r.locate, r.side, r.price, r.qty});
    }
    void on_executed(const engine::Executed& r) { settle(r.order_id, r.leaves); }
    void on_cancelled(const engine::Cancelled& r) { settle(r.order_id, r.leaves); }
    void on_replaced(const engine::Replaced& r) {
        const auto found = where_.find(r.old_id);
        if (found == where_.end()) {
            return;
        }
        Live order = live_[found->second];
        order.qty = r.qty;
        order.price = r.price;
        if (r.new_id == r.old_id) {
            live_[found->second] = order;
            return;
        }
        erase(found->second);
        order.id = r.new_id;
        insert(order);
    }
    void on_rejected(const engine::Rejected&) noexcept {}

    // --- Queries -------------------------------------------------------------

    // Orders the generator believes are resting. Equal to the engine's
    // open_orders() whenever the engine is between requests.
    [[nodiscard]] std::size_t live() const noexcept { return live_.size(); }
    [[nodiscard]] Nanos now() const noexcept { return now_; }
    [[nodiscard]] const FlowConfig& config() const noexcept { return cfg_; }

    // The mid price the generator is currently quoting around.
    [[nodiscard]] Price mid(Locate locate) const noexcept { return mids_[locate] * kTick; }

 private:
    struct Live {
        OrderId id;
        engine::OwnerId owner;
        Locate locate;
        Side side;
        Price price;
        Qty qty;
    };

    static constexpr std::uint32_t kMinMidTicks = 100;      // $1
    static constexpr std::uint32_t kMaxMidTicks = 400'000;  // $4000
    // No engine ever issues this id: it would take longer than the universe
    // has existed at a billion orders a second.
    static constexpr OrderId kNoSuchOrder = OrderId{1} << 62;

    Nanos tick_clock() {
        now_ += 1 + rng_.below(40'000);  // up to 40 microseconds apart
        return now_;
    }

    // --- What is resting -----------------------------------------------------

    void insert(const Live& order) {
        where_[order.id] = live_.size();
        live_.push_back(order);
    }

    void erase(std::size_t index) {
        where_.erase(live_[index].id);
        if (index + 1 != live_.size()) {
            live_[index] = live_.back();
            where_[live_[index].id] = index;
        }
        live_.pop_back();
    }

    void settle(OrderId id, Qty leaves) {
        const auto found = where_.find(id);
        if (found == where_.end()) {
            return;
        }
        if (leaves == 0) {
            erase(found->second);
        } else {
            live_[found->second].qty = leaves;
        }
    }

    [[nodiscard]] const Live& random_live() { return live_[rng_.below(live_.size())]; }

    // --- Building blocks -----------------------------------------------------

    // Low locates are busier than high ones, as a few names dominate a real
    // day. The minimum of two uniform draws is the cheapest skew there is.
    Locate random_locate() {
        const std::uint64_t a = rng_.below(cfg_.symbols);
        const std::uint64_t b = rng_.below(cfg_.symbols);
        return static_cast<Locate>(1 + std::min(a, b));
    }

    Side random_side() { return rng_.below(2) == 0 ? Side::Buy : Side::Sell; }

    // Mostly round lots, some odd lots, a few large orders.
    Qty random_qty() {
        const std::uint64_t roll = rng_.below(100);
        if (roll < 70) {
            return static_cast<Qty>(100 * rng_.between(1, 10));
        }
        if (roll < 90) {
            return static_cast<Qty>(rng_.between(1, 99));
        }
        return static_cast<Qty>(100 * rng_.between(11, 50));
    }

    // The symbol's mid in ticks, after its occasional one-tick step. Each
    // step leaves the orders resting on one side closer to the new mid than
    // new orders on the other side will be placed, so the steps are what make
    // passive orders trade. How often it steps sets how much of the feed is
    // executions.
    std::uint32_t stepped_mid(Locate locate) {
        std::uint32_t& mid = mids_[locate];
        const std::uint64_t roll = rng_.below(64);
        if (roll == 0 && mid > kMinMidTicks) {
            --mid;
        } else if (roll == 1 && mid < kMaxMidTicks) {
            ++mid;
        }
        return mid;
    }

    // How far behind the mid a passive order sits: most are near the touch.
    std::uint32_t passive_distance() {
        const std::uint64_t roll = rng_.below(100);
        if (roll < 40) {
            return 1;
        }
        if (roll < 65) {
            return 2;
        }
        if (roll < 80) {
            return 3;
        }
        if (roll < 90) {
            return static_cast<std::uint32_t>(rng_.between(4, 5));
        }
        return static_cast<std::uint32_t>(rng_.between(6, 20));
    }

    engine::NewOrder blank_order() {
        return engine::NewOrder{.owner = static_cast<engine::OwnerId>(rng_.between(1, cfg_.owners)),
                                .token = ++last_token_,
                                .locate = random_locate(),
                                .side = random_side(),
                                .qty = random_qty()};
    }

    // A price `ticks` behind the mid: below it for a buy, above it for a sell.
    [[nodiscard]] static Price behind(std::uint32_t mid, Side side, std::uint32_t ticks) {
        return (side == Side::Buy ? mid - ticks : mid + ticks) * kTick;
    }
    // A price `ticks` across the mid: where the other side is likely resting.
    [[nodiscard]] static Price across(std::uint32_t mid, Side side, std::uint32_t ticks) {
        return (side == Side::Buy ? mid + ticks : mid - ticks) * kTick;
    }

    static Command new_order(const engine::NewOrder& order) {
        return Command{.kind = CommandKind::New, .order = order};
    }

    // --- The requests --------------------------------------------------------

    Command passive_limit() {
        engine::NewOrder o = blank_order();
        o.price = behind(stepped_mid(o.locate), o.side, passive_distance());
        return new_order(o);
    }

    Command aggressive_limit(engine::TimeInForce tif) {
        engine::NewOrder o = blank_order();
        o.price = across(stepped_mid(o.locate), o.side, static_cast<std::uint32_t>(rng_.below(4)));
        o.tif = tif;
        return new_order(o);
    }

    Command market(engine::TimeInForce tif) {
        engine::NewOrder o = blank_order();
        o.kind = engine::OrderKind::Market;
        o.tif = tif;
        return new_order(o);
    }

    Command cancel() {
        const Live& order = random_live();
        return Command{.kind = CommandKind::Cancel, .owner = order.owner, .target = order.id};
    }

    Command replace() {
        const Live order = random_live();
        Command c{.kind = CommandKind::Replace,
                  .owner = order.owner,
                  .target = order.id,
                  .qty = order.qty,
                  .price = order.price};
        const std::uint64_t roll = rng_.below(100);
        if (roll < 40 || order.qty > 1'000'000) {
            // Smaller, same price: keeps its place in the queue.
            if (order.qty > 1) {
                c.qty = static_cast<Qty>(rng_.between(1, order.qty - 1));
            }
        } else if (roll < 65) {
            // Larger, same price: goes to the back.
            c.qty = order.qty + random_qty();
        } else {
            // A new price, one to three ticks either way. Towards the market
            // it may trade.
            const auto move = static_cast<Price>(rng_.between(1, 3)) * kTick;
            if (rng_.below(2) == 0) {
                c.price = order.price + move;
            } else if (order.price > move) {
                c.price = order.price - move;
            }
            if (rng_.below(2) == 0) {
                c.qty = random_qty();
            }
        }
        return c;
    }

    // A request that must be rejected.
    Command bad() {
        const std::uint64_t which = rng_.below(10);
        if (live_.empty() || which < 5) {
            engine::NewOrder o = blank_order();
            o.price = behind(mids_[o.locate], o.side, 1);
            switch (which) {
                case 0:
                    o.qty = 0;
                    break;
                case 1:
                    o.price = 0;
                    break;
                case 2:
                    // Neither 'B' nor 'S'. The cast is well defined: Side has a
                    // fixed underlying type.
                    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                    o.side = static_cast<Side>('X');
                    break;
                case 3:
                    o.locate = static_cast<Locate>(cfg_.symbols + 1U);  // never opened
                    break;
                default:
                    return Command{.kind = CommandKind::Cancel,
                                   .owner = o.owner,
                                   .target = kNoSuchOrder + last_token_};
            }
            return new_order(o);
        }
        const Live order = random_live();
        switch (which) {
            case 5:  // somebody else's order
                return Command{
                    .kind = CommandKind::Cancel, .owner = order.owner + 1, .target = order.id};
            case 6:
                return Command{.kind = CommandKind::Replace,
                               .owner = order.owner + 1,
                               .target = order.id,
                               .qty = order.qty,
                               .price = order.price};
            case 7:
                return Command{.kind = CommandKind::Replace,
                               .owner = order.owner,
                               .target = order.id,
                               .qty = 0,
                               .price = order.price};
            case 8:
                return Command{.kind = CommandKind::Replace,
                               .owner = order.owner,
                               .target = order.id,
                               .qty = order.qty,
                               .price = 0};
            default:
                return Command{.kind = CommandKind::Replace,
                               .owner = order.owner,
                               .target = kNoSuchOrder + last_token_,
                               .qty = order.qty,
                               .price = order.price};
        }
    }

    Command choose() {
        if (cfg_.bad_per_million != 0 && rng_.below(1'000'000) < cfg_.bad_per_million) {
            return bad();
        }
        // Below the target the generator mostly adds; above it, cancels win.
        // The number of resting orders therefore hovers around the target.
        const bool room = live_.size() < cfg_.target_live_orders;
        const std::uint64_t passive = room ? 560 : 330;
        const std::uint64_t roll = rng_.below(1000);
        if (roll < passive) {
            return passive_limit();
        }
        // Orders that reach across the mid are a small minority, as they are
        // on a real feed, where a few percent of messages are executions.
        if (roll < passive + 20) {
            return aggressive_limit(engine::TimeInForce::Day);
        }
        if (roll < passive + 30) {
            return market(engine::TimeInForce::Day);
        }
        if (roll < passive + 40) {
            return aggressive_limit(engine::TimeInForce::ImmediateOrCancel);
        }
        if (roll < passive + 44) {
            return aggressive_limit(engine::TimeInForce::FillOrKill);
        }
        if (roll < passive + 45) {
            return market(engine::TimeInForce::FillOrKill);
        }
        if (live_.empty()) {
            return passive_limit();
        }
        if (roll < passive + 155) {
            return replace();
        }
        return cancel();
    }

    FlowConfig cfg_;
    SplitMix64 rng_;
    std::vector<std::uint32_t> mids_;  // in ticks, indexed by locate
    std::vector<Live> live_;
    std::unordered_map<OrderId, std::size_t> where_;
    Nanos now_ = 34'200ULL * 1'000'000'000ULL;  // 09:30:00
    engine::Token last_token_ = 0;
};

}  // namespace obe::gen
