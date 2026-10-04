#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/engine/feed_writer.hpp"
#include "obe/engine/round_trip.hpp"
#include "obe/feed/parser.hpp"
#include "obe/gen/order_flow.hpp"
#include "support/engine_harness.hpp"
#include "support/flow_run.hpp"

// The round trip: success criterion 7.
//
//   engine -> market data -> bytes -> ItchParser -> BookManager
//
// The book at the far end is built by the phase 2 feed handler from the
// published bytes and nothing else. It must show the same shares at the same
// prices as the engine's own book. The engine and the feed handler were
// written separately and share only the message definitions, so each is a
// check on the other: if the engine published a cancel for the wrong amount,
// or the handler misread a replace, the two books would drift apart.

namespace {

using namespace obe;
using test::FlowRun;

using Mirror = book::BookManager<book::OrderStore, book::PriceLevels>;

template <class Impl>
class RoundTrip : public ::testing::Test {};
TYPED_TEST_SUITE(RoundTrip, test::EngineTypes);

// Feeds the bytes the writer has collected since the last call to the mirror.
template <class Run>
void drain(Run& run, Mirror& mirror) {
    const std::vector<std::byte> chunk = run.writer.take();
    feed::ItchParser parser(mirror);
    ASSERT_TRUE(parser.parse(chunk).ok());
}

// `locates` as in engine::compare_depth: how many locates to compare.
template <class Run>
void expect_books_equal(const Run& run, const Mirror& mirror, std::size_t locates) {
    const engine::DepthComparison depth = engine::compare_depth(*run.engine, mirror, locates);
    ASSERT_TRUE(depth.ok()) << depth.mismatched_sides << " sides differ; the first is locate "
                            << depth.bad_locate << (depth.bad_side == Side::Buy ? " buy" : " sell");
    ASSERT_EQ(mirror.orders().size(), run.engine->open_orders());
}

TYPED_TEST(RoundTrip, ThePublishedFeedRebuildsTheEnginesBook) {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        const gen::FlowConfig cfg{.seed = seed, .symbols = 12, .target_live_orders = 800};
        FlowRun<TypeParam> run(cfg);
        const auto mirror = std::make_unique<Mirror>();

        // Compared along the way, not only at the end: two wrongs that cancel
        // out by the close would otherwise go unnoticed.
        for (int checkpoint = 0; checkpoint < 40; ++checkpoint) {
            run.run(500);
            drain(run, *mirror);
            ASSERT_FALSE(this->HasFatalFailure());
            expect_books_equal(run, *mirror, cfg.symbols + 2U);
            ASSERT_FALSE(this->HasFatalFailure()) << "at checkpoint " << checkpoint;
        }
        // At the end, every locate there can be, opened or not.
        expect_books_equal(run, *mirror, std::size_t{1} << 16);
        ASSERT_FALSE(this->HasFatalFailure());

        // The feed handler saw nothing it had to tolerate.
        EXPECT_EQ(mirror->counters(), book::Counters{});
        EXPECT_TRUE(mirror->audit().clean());

        // A matching engine never leaves a book locked or crossed, so the
        // rebuilt book never is either, at any update.
        const book::Stats& stats = mirror->stats();
        EXPECT_EQ(stats.locked_while_trading, 0U);
        EXPECT_EQ(stats.crossed_while_trading, 0U);
        EXPECT_EQ(stats.locked_while_not_trading, 0U);
        EXPECT_EQ(stats.crossed_while_not_trading, 0U);

        // Both sides counted the same traded volume and know the same names.
        std::uint64_t executed = 0;
        std::uint64_t depth_levels = 0;
        for (std::uint32_t s = 1; s <= cfg.symbols; ++s) {
            const auto locate = static_cast<Locate>(s);
            executed += mirror->book(locate).executed_shares();
            depth_levels += mirror->book(locate).bids().size() + mirror->book(locate).asks().size();
            EXPECT_TRUE(mirror->listed(locate));
            EXPECT_EQ(mirror->symbol(locate), gen::flow_symbol(locate).view());
            EXPECT_EQ(mirror->book(locate).trading_state(), 'T');
        }
        EXPECT_EQ(executed, run.engine->stats().traded_shares);
        EXPECT_GT(depth_levels, 50U) << "the run should end with a book worth comparing";
    }
}

TYPED_TEST(RoundTrip, ABookManagerCanListenToTheEngineDirectly) {
    // BookManager is an ITCH handler, and the engine's market-data interface
    // is a subset of the ITCH handler interface, so the two plug together with
    // no bytes in between. Here the book is compared after every request.
    using Reports = engine::TeeReports<gen::OrderFlow, engine::NullReports>;
    using Engine = typename TypeParam::template Engine<Reports, Mirror>;

    const gen::FlowConfig cfg{.seed = 21, .symbols = 4, .target_live_orders = 200};
    gen::OrderFlow flow(cfg);
    engine::NullReports nobody;
    Reports reports(flow, nobody);
    const auto mirror = std::make_unique<Mirror>();
    const auto engine = std::make_unique<Engine>(reports, *mirror);
    flow.open(*engine);

    for (int i = 0; i < 5'000; ++i) {
        gen::apply(*engine, flow.next());
        for (std::uint32_t s = 1; s <= cfg.symbols; ++s) {
            const auto locate = static_cast<Locate>(s);
            ASSERT_EQ(mirror->book(locate).bids().best(), engine->best(locate, Side::Buy))
                << "request " << i;
            ASSERT_EQ(mirror->book(locate).asks().best(), engine->best(locate, Side::Sell))
                << "request " << i;
        }
        ASSERT_EQ(mirror->orders().size(), engine->open_orders()) << "request " << i;
    }
    EXPECT_TRUE(engine::compare_depth(*engine, *mirror).ok());
    EXPECT_EQ(mirror->counters(), book::Counters{});
}

TYPED_TEST(RoundTrip, TheComparisonNoticesABookThatDrifted) {
    FlowRun<TypeParam> run({.seed = 3, .symbols = 2, .target_live_orders = 50});
    const auto mirror = std::make_unique<Mirror>();
    run.run(500);
    drain(run, *mirror);
    ASSERT_TRUE(engine::compare_depth(*run.engine, *mirror).ok());

    // One share the engine never published.
    mirror->on_add(feed::AddOrder{.hdr = {.locate = 2, .tracking = 0, .timestamp = 1},
                                  .order_ref = OrderId{1} << 40,
                                  .side = Side::Sell,
                                  .shares = 1,
                                  .price = 4'000'000'000U});
    const engine::DepthComparison depth = engine::compare_depth(*run.engine, *mirror);
    EXPECT_FALSE(depth.ok());
    EXPECT_EQ(depth.mismatched_sides, 1U);
    EXPECT_EQ(depth.bad_locate, 2U);
    EXPECT_EQ(depth.bad_side, Side::Sell);
}

}  // namespace
