#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/book/types.hpp"
#include "obe/engine/engines.hpp"
#include "obe/feed/handler.hpp"
#include "obe/feed/messages.hpp"
#include "obe/feed/parser.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/sim/simulator.hpp"
#include "obe/sim/strategy.hpp"
#include "obe/sim/types.hpp"
#include "obe/types.hpp"
#include "support/flow_run.hpp"
#include "support/printers.hpp"
#include "support/sim_harness.hpp"

// The simulator over generated markets.
//
// The scenario tests pin each rule of the fill model on a handful of messages.
// These run whole markets through it: the feed a matching engine published
// while seeded random flow traded on it (the same feed phase 5's round trip
// uses), tens of thousands of messages at a time. After every message the
// test checks what must always hold, against bookkeeping it keeps itself:
//
//   1. The queue ahead of each quote equals the shares left in the orders
//      that were at its price when it joined. The test finds those orders
//      from its own copy of the book, so this checks the simulator's
//      counting of who is ahead and who is behind, not its arithmetic.
//   2. A fill is at the quote's price, for no more than the quote had, at the
//      time of the message that caused it.
//   3. The profit figures agree with each other and with a recount from the
//      list of fills.
//   4. The book inside the simulator is the book a plain replay builds.

namespace {

using namespace obe;

// --- The feed ------------------------------------------------------------------

constexpr Locate kLocate = 1;

// The market data of a busy little market, as the bytes of an ITCH file.
std::vector<std::byte> make_feed(std::uint64_t seed, std::uint64_t commands) {
    gen::FlowConfig cfg;
    cfg.seed = seed;
    cfg.symbols = 2;
    cfg.owners = 6;
    cfg.target_live_orders = 60;
    cfg.bad_per_million = 20'000;
    test::FlowRun<engine::ReferenceEngineImpl> run(cfg);
    run.run(commands);
    return run.writer.take();
}

std::string symbol_name() {
    return std::string(gen::flow_symbol(kLocate).view());
}

// --- The test's own copy of the quoted security's orders -------------------------

struct RealOrder {
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
};

// Follows the same messages as the simulator, for one security, and knows
// nothing about queues or quotes.
struct Oracle : feed::HandlerBase {
    std::unordered_map<OrderId, RealOrder> live;
    Nanos first = 0;    // time of the security's first message
    Nanos now = 0;      // and of its latest
    bool ours = false;  // was the last message for the quoted security?

    void note(const feed::Header& hdr) {
        ours = hdr.locate == kLocate;
        if (ours) {
            if (first == 0) {
                first = hdr.timestamp;
            }
            now = hdr.timestamp;
        }
    }
    void reduce(OrderId ref, Qty shares) {
        const auto it = live.find(ref);
        if (it == live.end()) {
            return;
        }
        it->second.qty -= shares < it->second.qty ? shares : it->second.qty;
        if (it->second.qty == 0) {
            live.erase(it);
        }
    }

    void on_trading_action(const feed::TradingAction& m) { note(m.hdr); }
    void on_add(const feed::AddOrder& m) {
        note(m.hdr);
        if (ours && m.shares != 0) {
            live.try_emplace(m.order_ref, RealOrder{m.side, m.price, m.shares});
        }
    }
    void on_execute(const feed::OrderExecuted& m) {
        note(m.hdr);
        if (ours) {
            reduce(m.order_ref, m.shares);
        }
    }
    void on_execute_with_price(const feed::OrderExecutedWithPrice& m) {
        note(m.hdr);
        if (ours) {
            reduce(m.order_ref, m.shares);
        }
    }
    void on_cancel(const feed::OrderCancel& m) {
        note(m.hdr);
        if (ours) {
            reduce(m.order_ref, m.shares);
        }
    }
    void on_delete(const feed::OrderDelete& m) {
        note(m.hdr);
        if (ours) {
            live.erase(m.order_ref);
        }
    }
    void on_replace(const feed::OrderReplace& m) {
        note(m.hdr);
        if (!ours) {
            return;
        }
        const auto it = live.find(m.orig_order_ref);
        if (it == live.end()) {
            return;
        }
        const Side side = it->second.side;
        live.erase(it);
        if (m.shares != 0) {
            live.try_emplace(m.new_order_ref, RealOrder{side, m.price, m.shares});
        }
    }
    // Every other message type: not for the quoted security's book.
    void on_system_event(const feed::SystemEvent& /*m*/) { ours = false; }
    void on_stock_directory(const feed::StockDirectory& /*m*/) { ours = false; }

    // The orders resting at one price on one side, right now.
    [[nodiscard]] std::vector<OrderId> at(Side side, Price price) const {
        std::vector<OrderId> out;
        for (const auto& [ref, order] : live) {
            if (order.side == side && order.price == price) {
                out.push_back(ref);
            }
        }
        return out;
    }
    [[nodiscard]] std::uint64_t left(const std::vector<OrderId>& refs) const {
        std::uint64_t total = 0;
        for (const OrderId ref : refs) {
            const auto it = live.find(ref);
            if (it != live.end()) {
                total += it->second.qty;
            }
        }
        return total;
    }
};

// --- One run, checked message by message ---------------------------------------

struct Outcome {
    sim::SimReport report;
    std::vector<sim::Fill> fills;
    std::uint64_t quotes_followed = 0;  // placements whose queue the test tracked
    std::uint64_t ours = 0;             // messages for the quoted security
    std::uint64_t max_ahead = 0;
    std::int64_t worst_long = 0;
    std::int64_t worst_short = 0;
};

template <sim::Strategy S>
Outcome run_checked(std::span<const std::byte> feed, S strategy, sim::SimConfig cfg) {
    cfg.symbol = symbol_name();
    sim::MarketMakingSim simulator(cfg, strategy);
    using Sim = sim::MarketMakingSim<S>;
    feed::ItchParser<Sim> to_sim(simulator);
    Oracle oracle;
    feed::ItchParser<Oracle> to_oracle(oracle);

    Outcome out;
    // For each side: the real orders that were at our price when we joined.
    std::array<std::vector<OrderId>, 2> ahead_of_us;
    const std::array<Side, 2> sides{Side::Buy, Side::Sell};

    feed::FrameReader reader(feed);
    feed::Frame frame;
    std::uint64_t message = 0;
    while (!reader.done()) {
        if (reader.next(frame) != feed::ParseStatus::Ok) {
            ADD_FAILURE() << "the feed is not well formed";
            return out;
        }
        ++message;
        const std::array<std::optional<sim::RestingQuote>, 2> before{simulator.working(Side::Buy),
                                                                     simulator.working(Side::Sell)};
        const std::size_t fills_before = simulator.fills().size();

        // With latency, new quotes land before the message is applied, so
        // they join the queue as it stood before it. Without, they are placed
        // after it. The test's copy of the book has to be read at the same
        // moment, which is why the order of these steps depends on it.
        const bool lands_first = cfg.latency != 0;
        if (to_sim.dispatch(frame) != feed::ParseStatus::Ok) {
            ADD_FAILURE() << "message " << message << " did not parse";
            return out;
        }
        if (!lands_first) {
            static_cast<void>(to_oracle.dispatch(frame));
        }

        // Fills caused by this message, per side.
        std::array<std::uint64_t, 2> filled{};
        for (std::size_t i = fills_before; i < simulator.fills().size(); ++i) {
            const sim::Fill& f = simulator.fills()[i];
            const std::size_t s = f.side == Side::Buy ? 0 : 1;
            filled[s] += f.qty;
            if (f.qty == 0) {
                ADD_FAILURE() << "message " << message << ": an empty fill";
            }
            if (!lands_first) {
                // The quote that traded is the one that was resting before.
                if (!before[s].has_value() || f.price != before[s]->price) {
                    ADD_FAILURE() << "message " << message << ": a fill at " << f.price
                                  << " with no quote resting there";
                    return out;
                }
            }
        }
        for (std::size_t s = 0; s < 2; ++s) {
            if (!lands_first && before[s].has_value() && filled[s] > before[s]->qty) {
                ADD_FAILURE() << "message " << message << ": filled " << filled[s]
                              << " of a quote for " << before[s]->qty;
                return out;
            }
            const std::optional<sim::RestingQuote> after = simulator.working(sides[s]);
            if (!after.has_value()) {
                continue;
            }
            const bool same_quote = before[s].has_value() && after->price == before[s]->price &&
                                    after->qty + filled[s] == before[s]->qty;
            if (!same_quote) {
                // A new quote: note who is in front of it.
                ahead_of_us[s] = oracle.at(sides[s], after->price);
                ++out.quotes_followed;
            }
        }
        if (lands_first) {
            static_cast<void>(to_oracle.dispatch(frame));
        }

        // (1) The queue ahead is exactly what is left of those orders.
        for (std::size_t s = 0; s < 2; ++s) {
            const std::optional<sim::RestingQuote> after = simulator.working(sides[s]);
            if (!after.has_value()) {
                continue;
            }
            const std::uint64_t expected = oracle.left(ahead_of_us[s]);
            if (after->ahead != expected) {
                ADD_FAILURE() << "message " << message << ", " << (s == 0 ? "bid" : "ask") << " at "
                              << after->price << ": " << after->ahead
                              << " ahead, but the orders that were there hold " << expected;
                return out;
            }
            out.max_ahead = std::max(out.max_ahead, after->ahead);
        }
        if (oracle.ours) {
            ++out.ours;
            // (2) A fill carries the time of its message.
            for (std::size_t i = fills_before; i < simulator.fills().size(); ++i) {
                if (simulator.fills()[i].time != oracle.now) {
                    ADD_FAILURE() << "message " << message << ": a fill stamped "
                                  << simulator.fills()[i].time << " at " << oracle.now;
                    return out;
                }
            }
        } else if (simulator.fills().size() != fills_before) {
            ADD_FAILURE() << "message " << message << " is for another security and caused a fill";
            return out;
        }
        // (3) The money adds up after every message.
        const sim::SimReport r = simulator.report();
        if (!r.consistent()) {
            ADD_FAILURE() << "message " << message << ": the report contradicts itself";
            return out;
        }
        out.worst_long = std::max(out.worst_long, r.inventory);
        out.worst_short = std::max(out.worst_short, -r.inventory);
    }
    simulator.finish();
    out.report = simulator.report();
    out.fills = simulator.fills();

    // (4) The book inside is the book a plain replay builds.
    book::BookManager<book::OrderStore, book::PriceLevels> plain;
    feed::ItchParser plain_parser(plain);
    EXPECT_TRUE(plain_parser.parse(feed).ok());
    EXPECT_EQ(simulator.book().counters(), plain.counters());
    EXPECT_EQ(simulator.book().stats(), plain.stats());
    EXPECT_EQ(simulator.book().orders().size(), plain.orders().size());
    for (Locate locate = 0; locate < 8; ++locate) {
        EXPECT_EQ(simulator.book().book(locate).bbo(), plain.book(locate).bbo()) << locate;
        EXPECT_EQ(simulator.book().book(locate).executed_shares(),
                  plain.book(locate).executed_shares());
    }
    // And the last mid the report holds is that book's.
    const book::Bbo bbo = plain.book(kLocate).bbo();
    if (bbo.has_bid() && bbo.has_ask()) {
        EXPECT_EQ(out.report.mid2, static_cast<std::int64_t>(bbo.bid_price) + bbo.ask_price);
    }
    return out;
}

// Recounts the report's money from the list of fills alone.
void expect_recount_matches(const Outcome& out, std::int64_t rebate) {
    std::uint64_t bought = 0;
    std::uint64_t sold = 0;
    std::int64_t cash = 0;
    std::int64_t spread2 = 0;
    std::array<std::uint64_t, 3> by_reason{};
    for (const sim::Fill& f : out.fills) {
        const auto qty = static_cast<std::int64_t>(f.qty);
        const auto price = static_cast<std::int64_t>(f.price);
        if (f.side == Side::Buy) {
            bought += f.qty;
            cash -= price * qty;
            spread2 += qty * (f.mid2 - 2 * price);
        } else {
            sold += f.qty;
            cash += price * qty;
            spread2 += qty * (2 * price - f.mid2);
        }
        cash += rebate * qty;
        ++by_reason[static_cast<std::size_t>(f.reason)];
    }
    const sim::SimReport& r = out.report;
    EXPECT_EQ(r.fills, out.fills.size());
    EXPECT_EQ(r.bought, bought);
    EXPECT_EQ(r.sold, sold);
    EXPECT_EQ(r.inventory, static_cast<std::int64_t>(bought) - static_cast<std::int64_t>(sold));
    EXPECT_EQ(r.cash, cash);
    EXPECT_EQ(r.spread2, spread2);
    EXPECT_EQ(r.rebates, rebate * static_cast<std::int64_t>(bought + sold));
    EXPECT_EQ(r.pnl2, 2 * cash + r.inventory * r.mid2);
    EXPECT_EQ(r.fills_queue, by_reason[0]);
    EXPECT_EQ(r.fills_through, by_reason[1]);
    EXPECT_EQ(r.fills_crossed, by_reason[2]);
    EXPECT_EQ(r.max_long, out.worst_long);
    EXPECT_EQ(r.max_short, out.worst_short);
    EXPECT_GE(r.max_drawdown2, 0);
    for (const sim::Markout& m : r.markouts) {
        EXPECT_EQ(m.shares + m.unmeasured_shares, bought + sold) << "horizon " << m.horizon;
    }
    EXPECT_GE(r.placed, r.fills == 0 ? 0U : 1U);
    EXPECT_TRUE(r.consistent());
}

constexpr std::uint64_t kCommands = 30'000;

// --- The properties ------------------------------------------------------------

class SimProperty : public ::testing::TestWithParam<std::uint64_t> {
 protected:
    void SetUp() override { feed = make_feed(GetParam(), kCommands); }
    std::vector<std::byte> feed;
};

TEST_P(SimProperty, JoiningTheBestQueueHoldsEveryInvariant) {
    const sim::JoinBest strategy{.qty = 100, .max_inventory = 500};
    const Outcome out = run_checked(feed, strategy, {});
    expect_recount_matches(out, 0);
    EXPECT_GT(out.quotes_followed, 100U);
    EXPECT_GT(out.max_ahead, 0U) << "no quote ever had anybody ahead of it";
    EXPECT_GT(out.report.fills_queue, 0U);
    // A quote is placed only below the limit, so the position can pass it by
    // less than one quote.
    EXPECT_LT(out.worst_long, 500 + 100);
    EXPECT_LT(out.worst_short, 500 + 100);
}

TEST_P(SimProperty, QuotingInsideTheSpreadHoldsEveryInvariant) {
    const sim::FixedSpread strategy{.half_spread = 100, .qty = 100, .max_inventory = 300};
    const Outcome out = run_checked(feed, strategy, {});
    expect_recount_matches(out, 0);
    EXPECT_GT(out.quotes_followed, 100U);
    EXPECT_LT(out.worst_long, 300 + 100);
    EXPECT_LT(out.worst_short, 300 + 100);
}

TEST_P(SimProperty, AWideQuoteHoldsEveryInvariant) {
    const sim::FixedSpread strategy{.half_spread = 400, .qty = 250, .max_inventory = 1'000};
    sim::SimConfig cfg;
    cfg.rebate = 25;
    const Outcome out = run_checked(feed, strategy, cfg);
    expect_recount_matches(out, 25);
}

TEST_P(SimProperty, WithLatencyEveryInvariantStillHolds) {
    sim::SimConfig cfg;
    cfg.latency = 150;  // a few messages' worth at this feed's pace
    Outcome out = run_checked(feed, sim::JoinBest{.qty = 100, .max_inventory = 500}, cfg);
    expect_recount_matches(out, 0);
    EXPECT_GT(out.quotes_followed, 100U);

    cfg.latency = 5'000;
    cfg.rebate = -3;  // a fee
    out = run_checked(feed, sim::FixedSpread{.half_spread = 100, .qty = 100, .max_inventory = 300},
                      cfg);
    expect_recount_matches(out, -3);
}

TEST_P(SimProperty, AWindowInTheMiddleOfTheFileIsRespected) {
    // Quote only during the middle third of the run.
    Oracle clock;
    feed::ItchParser<Oracle> parser(clock);
    ASSERT_TRUE(parser.parse(feed).ok());
    const Nanos third = (clock.now - clock.first) / 3;
    ASSERT_GT(third, 0U);
    sim::SimConfig cfg;
    cfg.start = clock.first + third;
    cfg.end = clock.first + 2 * third;
    const Outcome out = run_checked(feed, sim::JoinBest{.qty = 100, .max_inventory = 500}, cfg);
    expect_recount_matches(out, 0);
    ASSERT_FALSE(out.fills.empty());
    for (const sim::Fill& f : out.fills) {
        ASSERT_GE(f.time, cfg.start);
        ASSERT_LT(f.time, cfg.end);
    }
    // Roughly a third of the day's trading happened inside it.
    const Outcome whole = run_checked(feed, sim::NoQuotes{}, {});
    EXPECT_GT(out.report.market_executed, whole.report.market_executed / 6);
    EXPECT_LT(out.report.market_executed, whole.report.market_executed / 2);
}

TEST_P(SimProperty, AStrategyThatNeverQuotesIsAPlainReplay) {
    const Outcome out = run_checked(feed, sim::NoQuotes{}, {});
    EXPECT_TRUE(out.fills.empty());
    EXPECT_EQ(out.report.placed, 0U);
    EXPECT_EQ(out.report.cash, 0);
    EXPECT_EQ(out.report.pnl2, 0);
    EXPECT_EQ(out.report.inventory, 0);
    EXPECT_EQ(out.report.max_drawdown2, 0);
    EXPECT_GT(out.report.decisions, 0U) << "it was asked; it declined";
    EXPECT_GT(out.report.market_executed, 0U);
}

TEST_P(SimProperty, TheSameInputsGiveTheSameResult) {
    const sim::FixedSpread strategy{.half_spread = 100, .qty = 100, .max_inventory = 300};
    sim::SimConfig cfg;
    cfg.latency = 150;
    const Outcome first = run_checked(feed, strategy, cfg);
    const Outcome second = run_checked(feed, strategy, cfg);
    EXPECT_EQ(first.report, second.report);
    EXPECT_EQ(first.fills, second.fills);
    // And a different feed gives a different one: the comparison can fail.
    const std::vector<std::byte> other = make_feed(GetParam() + 1'000, kCommands);
    EXPECT_NE(run_checked(other, strategy, cfg).fills, first.fills);
}

INSTANTIATE_TEST_SUITE_P(Seeds, SimProperty, ::testing::Values(1, 2, 3, 4));

// The properties above are only worth something if the runs take every path
// through the fill model. This checks that they do, across the seeds.
TEST(SimPropertyCoverage, TheRunsReachEveryKindOfFillAndRefusal) {
    sim::SimReport total;
    std::uint64_t in_flight_runs = 0;
    for (const std::uint64_t seed : {1U, 2U, 3U, 4U}) {
        const std::vector<std::byte> feed = make_feed(seed, kCommands);
        sim::SimConfig cfg;
        cfg.latency = 5'000;
        for (const Price half : {Price{100}, Price{400}}) {
            const Outcome out = run_checked(
                feed, sim::FixedSpread{.half_spread = half, .qty = 100, .max_inventory = 300}, cfg);
            total.fills_queue += out.report.fills_queue;
            total.fills_through += out.report.fills_through;
            total.fills_crossed += out.report.fills_crossed;
            total.rejected_crossing += out.report.rejected_crossing;
            total.cancelled += out.report.cancelled;
            total.bought += out.report.bought;
            total.sold += out.report.sold;
            ++in_flight_runs;
        }
        const Outcome join = run_checked(feed, sim::JoinBest{.qty = 100, .max_inventory = 500}, {});
        total.fills_queue += join.report.fills_queue;
    }
    EXPECT_GT(total.fills_queue, 0U);
    EXPECT_GT(total.fills_through, 0U);
    EXPECT_GT(total.fills_crossed, 0U);
    EXPECT_GT(total.rejected_crossing, 0U) << "no quote ever arrived to find the market on it";
    EXPECT_GT(total.cancelled, 0U);
    EXPECT_GT(total.bought, 0U);
    EXPECT_GT(total.sold, 0U);
    EXPECT_EQ(in_flight_runs, 8U);
}

}  // namespace
