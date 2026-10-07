#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "obe/engine/concepts.hpp"
#include "obe/engine/feed_writer.hpp"
#include "obe/engine/reference_engine.hpp"
#include "obe/engine/types.hpp"
#include "obe/gen/order_flow.hpp"
#include "support/engine_harness.hpp"
#include "support/flow_run.hpp"

// The order-flow generator. These tests run it against the reference engine,
// since a generator that follows the book through reports cannot be exercised
// without something to send the reports.

namespace {

using namespace obe;
using gen::Command;
using gen::CommandKind;
using RefRun = test::FlowRun<engine::ReferenceEngineImpl>;

std::vector<Command> commands_of(const gen::FlowConfig& cfg, std::size_t count) {
    RefRun run(cfg);
    std::vector<Command> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(run.step());
    }
    return out;
}

TEST(OrderFlow, TheSameConfigGivesTheSameRequests) {
    const gen::FlowConfig cfg{.seed = 42, .symbols = 5, .target_live_orders = 300};
    EXPECT_EQ(commands_of(cfg, 5'000), commands_of(cfg, 5'000));
}

// What the generator produces for a seed is something other things rest on:
// the tapes two builds are benchmarked on, and the seed a failing test is
// reported by. So it is pinned to a number, which catches two things.
//
// Instructions were added later, and must not have moved it: with every
// instruction rate at zero, which is the default, the requests are field for
// field what they were before instructions existed. The two numbers below
// were taken from that version of the generator, built with GCC.
//
// And the number is the same under every compiler. It was not always: two
// draws were once made in the arguments of one call, an order the language
// leaves open, and Clang's flow differed from GCC's. This test is what found
// it.
TEST(OrderFlow, WithoutInstructionsTheRequestsAreWhatTheyWereBeforeThereWereAny) {
    const auto hash_of = [](const gen::FlowConfig& cfg) {
        std::uint64_t h = 0xcbf29ce484222325ULL;
        const auto mix = [&h](std::uint64_t v) {
            h ^= v;
            h *= 0x100000001b3ULL;
            h ^= h >> 32;
        };
        for (const Command& c : commands_of(cfg, 20'000)) {
            mix(static_cast<std::uint64_t>(c.kind));
            mix(c.now);
            mix(c.order.owner);
            mix(c.order.token);
            mix(c.order.locate);
            mix(static_cast<std::uint64_t>(c.order.side));
            mix(c.order.qty);
            mix(c.order.price);
            mix(static_cast<std::uint64_t>(c.order.kind));
            mix(static_cast<std::uint64_t>(c.order.tif));
            mix(c.owner);
            mix(c.target);
            mix(c.qty);
            mix(c.price);
            EXPECT_EQ(c.order.display, 0U);
            EXPECT_FALSE(c.order.post_only);
            EXPECT_EQ(c.order.self_match, engine::SelfMatch::Allow);
        }
        return h;
    };
    EXPECT_EQ(hash_of({.seed = 42, .symbols = 5, .target_live_orders = 300}),
              0xa0221005217acadbULL);
    EXPECT_EQ(
        hash_of({.seed = 7, .symbols = 6, .target_live_orders = 250, .bad_per_million = 20'000}),
        0xe2406406faedf7b1ULL);
}

TEST(OrderFlow, ADifferentSeedGivesDifferentRequests) {
    gen::FlowConfig a{.seed = 42, .symbols = 5, .target_live_orders = 300};
    gen::FlowConfig b = a;
    b.seed = 43;
    EXPECT_NE(commands_of(a, 200), commands_of(b, 200));
}

TEST(OrderFlow, KnowsWhatIsRestingFromTheReportsAlone) {
    RefRun run({.seed = 7, .symbols = 6, .target_live_orders = 250, .bad_per_million = 20'000});
    for (int i = 0; i < 20'000; ++i) {
        run.step();
        ASSERT_EQ(run.flow.live(), run.engine->open_orders()) << "after request " << i;
    }
}

TEST(OrderFlow, RestingOrdersHoverAroundTheTarget) {
    constexpr std::uint32_t kTarget = 1'000;
    RefRun run({.seed = 3, .symbols = 8, .target_live_orders = kTarget});
    run.run(20'000);  // long enough to fill the book from empty
    std::size_t lowest = run.flow.live();
    std::size_t highest = run.flow.live();
    for (int i = 0; i < 20'000; ++i) {
        run.step();
        lowest = std::min(lowest, run.flow.live());
        highest = std::max(highest, run.flow.live());
    }
    EXPECT_GT(lowest, kTarget * 9 / 10);
    EXPECT_LT(highest, kTarget * 11 / 10);
}

TEST(OrderFlow, TimeMovesForwardWithEveryRequest) {
    RefRun run({.seed = 5});
    Nanos last = run.flow.now();
    EXPECT_GE(last, 34'200ULL * 1'000'000'000ULL) << "the day starts at 09:30";
    for (int i = 0; i < 2'000; ++i) {
        const Command c = run.step();
        ASSERT_GT(c.now, last);
        last = c.now;
    }
    EXPECT_EQ(run.flow.now(), last);
}

TEST(OrderFlow, AsksForEveryKindOfOrder) {
    const std::vector<Command> commands =
        commands_of({.seed = 9, .symbols = 4, .target_live_orders = 200}, 20'000);
    std::size_t limit_day = 0;
    std::size_t limit_ioc = 0;
    std::size_t limit_fok = 0;
    std::size_t market = 0;
    std::size_t market_fok = 0;
    std::size_t cancels = 0;
    std::size_t replaces = 0;
    std::size_t buys = 0;
    std::size_t sells = 0;
    for (const Command& c : commands) {
        if (c.kind == CommandKind::Cancel) {
            ++cancels;
        } else if (c.kind == CommandKind::Replace) {
            ++replaces;
        } else if (c.order.kind == engine::OrderKind::Market) {
            ++(c.order.tif == engine::TimeInForce::FillOrKill ? market_fok : market);
        } else if (c.order.tif == engine::TimeInForce::Day) {
            ++limit_day;
        } else if (c.order.tif == engine::TimeInForce::ImmediateOrCancel) {
            ++limit_ioc;
        } else {
            ++limit_fok;
        }
        if (c.kind == CommandKind::New) {
            ++(c.order.side == Side::Buy ? buys : sells);
        }
    }
    EXPECT_GT(limit_day, 5'000U);
    EXPECT_GT(limit_ioc, 100U);
    EXPECT_GT(limit_fok, 30U);
    EXPECT_GT(market, 100U);
    EXPECT_GT(market_fok, 5U);
    EXPECT_GT(cancels, 3'000U);
    EXPECT_GT(replaces, 1'500U);
    // Neither side dominates.
    EXPECT_GT(buys, sells * 9 / 10);
    EXPECT_GT(sells, buys * 9 / 10);
}

TEST(OrderFlow, ValidRequestsAreWellFormed) {
    const gen::FlowConfig cfg{
        .seed = 13, .symbols = 4, .owners = 5, .target_live_orders = 200, .bad_per_million = 0};
    std::set<engine::Token> tokens;
    for (const Command& c : commands_of(cfg, 10'000)) {
        if (c.kind == CommandKind::New) {
            ASSERT_GE(c.order.locate, 1U);
            ASSERT_LE(c.order.locate, cfg.symbols);
            ASSERT_GE(c.order.owner, 1U);
            ASSERT_LE(c.order.owner, cfg.owners);
            ASSERT_GT(c.order.qty, 0U);
            ASSERT_TRUE(c.order.side == Side::Buy || c.order.side == Side::Sell);
            ASSERT_TRUE(tokens.insert(c.order.token).second) << "tokens must not repeat";
            if (c.order.kind == engine::OrderKind::Limit) {
                ASSERT_GT(c.order.price, 0U);
                ASSERT_EQ(c.order.price % gen::OrderFlow::kTick, 0U) << "prices are whole cents";
            }
        } else {
            ASSERT_NE(c.target, 0U);
            ASSERT_GE(c.owner, 1U);
            ASSERT_LE(c.owner, cfg.owners);
        }
        if (c.kind == CommandKind::Replace) {
            ASSERT_GT(c.qty, 0U);
            ASSERT_GT(c.price, 0U);
            ASSERT_EQ(c.price % gen::OrderFlow::kTick, 0U);
        }
    }
}

TEST(OrderFlow, NothingIsRejectedWhenBadRequestsAreOff) {
    RefRun run({.seed = 17, .symbols = 4, .target_live_orders = 200, .bad_per_million = 0});
    run.run(30'000);
    EXPECT_EQ(run.engine->stats().rejected, 0U);
    EXPECT_TRUE(run.log.of<engine::Rejected>().empty());
}

TEST(OrderFlow, BadRequestsAreRejectedAndChangeNothing) {
    // Every request is bad: the engine must refuse them all, so the book
    // stays empty and no id is ever issued.
    RefRun run({.seed = 19, .symbols = 4, .target_live_orders = 200, .bad_per_million = 1'000'000});
    run.run(5'000);
    EXPECT_EQ(run.engine->stats().rejected, 5'000U);
    EXPECT_EQ(run.engine->stats().accepted, 0U);
    EXPECT_EQ(run.engine->open_orders(), 0U);
    EXPECT_EQ(run.log.all.size(), 5'000U);
    // Only the opening Stock Directory and Trading Action messages.
    EXPECT_EQ(run.messages.all.size(), 8U);
}

TEST(OrderFlow, BadRequestsCoverEveryRejectReasonOnceOrdersAreResting) {
    RefRun run({.seed = 23, .symbols = 4, .target_live_orders = 100, .bad_per_million = 200'000});
    run.run(10'000);
    std::set<engine::RejectReason> seen;
    for (const engine::Rejected& r : run.log.of<engine::Rejected>()) {
        seen.insert(r.reason);
    }
    EXPECT_EQ(seen.size(), 6U);
    // About one request in five was bad, and every bad one was refused.
    EXPECT_GT(run.engine->stats().rejected, 1'700U);
    EXPECT_LT(run.engine->stats().rejected, 2'300U);
}

TEST(OrderFlow, OpensEverySymbolUnderItsName) {
    RefRun run({.seed = 1, .symbols = 3});
    EXPECT_EQ(run.messages.trace(), "RHRHRH");
    const std::vector<feed::StockDirectory> names = run.messages.of<feed::StockDirectory>();
    ASSERT_EQ(names.size(), 3U);
    EXPECT_EQ(names[0].stock.view(), "S00001");
    EXPECT_EQ(names[2].stock.view(), "S00003");
    EXPECT_EQ(names[2].hdr.locate, 3U);
    EXPECT_FALSE(run.engine->listed(4));
}

TEST(OrderFlow, QuotesAroundAMidThatMoves) {
    RefRun run({.seed = 29, .symbols = 2, .target_live_orders = 100});
    const Price start = run.flow.mid(1);
    EXPECT_EQ(start % gen::OrderFlow::kTick, 0U);
    run.run(30'000);
    EXPECT_NE(run.flow.mid(1), start) << "the mid should have wandered";
    // The book straddles it: the best bid is not far above, the best offer not
    // far below.
    const auto bid = run.engine->best(1, Side::Buy);
    const auto ask = run.engine->best(1, Side::Sell);
    ASSERT_TRUE(bid && ask);
    const Price mid = run.flow.mid(1);
    EXPECT_LT(bid->price, mid + 10 * gen::OrderFlow::kTick);
    EXPECT_GT(ask->price + 10 * gen::OrderFlow::kTick, mid);
}

TEST(OrderFlow, OutOfRangeConfigIsClamped) {
    gen::OrderFlow none({.symbols = 0, .owners = 0, .target_live_orders = 0});
    EXPECT_EQ(none.config().symbols, 1U);
    EXPECT_EQ(none.config().owners, 1U);
    EXPECT_EQ(none.config().target_live_orders, 1U);
    gen::OrderFlow many({.symbols = 1'000'000});
    EXPECT_EQ(many.config().symbols, 60'000U);
}

TEST(FlowSymbol, IsTheLetterSAndFiveDigits) {
    EXPECT_EQ(gen::flow_symbol(1).view(), "S00001");
    EXPECT_EQ(gen::flow_symbol(42).view(), "S00042");
    EXPECT_EQ(gen::flow_symbol(60'000).view(), "S60000");
    EXPECT_EQ(gen::flow_symbol(65'535).view(), "S65535");
}

TEST(Apply, RoutesEachKindOfCommandToItsOperation) {
    test::ReportLog reports;
    engine::NullMarketData nobody;
    const auto engine =
        std::make_unique<engine::ReferenceEngine<test::ReportLog, engine::NullMarketData>>(reports,
                                                                                           nobody);
    engine->add_instrument(1, gen::flow_symbol(1), 1);

    gen::apply(*engine, Command{.kind = CommandKind::New,
                                .now = 10,
                                .order = {.owner = 4,
                                          .token = 9,
                                          .locate = 1,
                                          .side = Side::Buy,
                                          .qty = 300,
                                          .price = 1'000'000}});
    gen::apply(*engine, Command{.kind = CommandKind::Replace,
                                .now = 11,
                                .owner = 4,
                                .target = 1,
                                .qty = 100,
                                .price = 1'000'000});
    gen::apply(*engine, Command{.kind = CommandKind::Cancel, .now = 12, .owner = 4, .target = 1});

    ASSERT_EQ(reports.all.size(), 3U);
    EXPECT_EQ(std::get<engine::Accepted>(reports.all[0]).timestamp, 10U);
    EXPECT_EQ(std::get<engine::Replaced>(reports.all[1]).qty, 100U);
    EXPECT_EQ(std::get<engine::Replaced>(reports.all[1]).timestamp, 11U);
    EXPECT_EQ(std::get<engine::Cancelled>(reports.all[2]).qty, 100U);
    EXPECT_EQ(std::get<engine::Cancelled>(reports.all[2]).timestamp, 12U);
}

}  // namespace
