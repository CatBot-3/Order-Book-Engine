#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "obe/engine/engines.hpp"
#include "obe/engine/feed_writer.hpp"
#include "obe/engine/output_hash.hpp"
#include "obe/gen/order_flow.hpp"
#include "support/engine_harness.hpp"
#include "support/flow_run.hpp"

// The differential test for the hand-written engine.
//
// MatchingEngine and ReferenceEngine are given the same seeded flow. They must
// say exactly the same things: the same market data, byte for byte, and the
// same reports, one by one. That is stricter than "the same trades": it also
// pins the order of everything published, the ids, the match numbers and the
// timestamps.
//
// This is what makes it safe to optimize the engine. A change that makes it
// faster by getting a corner wrong fails here, and the message says at which
// request the two first disagreed. scripts/diff_engines.sh makes the same
// comparison through flow_gen on runs too long to keep in memory.

namespace {

using namespace obe;
using test::FlowRun;

template <class Impl>
class EngineDifferential : public ::testing::Test {};
TYPED_TEST_SUITE(EngineDifferential, test::EngineTypes);

template <class Impl>
void expect_same_as_reference(const gen::FlowConfig& cfg, std::uint64_t commands) {
    FlowRun<engine::ReferenceEngineImpl> reference(cfg);
    FlowRun<Impl> candidate(cfg);

    for (std::uint64_t i = 0; i < commands; ++i) {
        const std::size_t reports_before = reference.log.all.size();
        const std::size_t messages_before = reference.messages.all.size();
        const gen::Command asked = reference.step();
        ASSERT_EQ(candidate.step(), asked) << "the engines diverged before request " << i;

        ASSERT_EQ(candidate.log.all.size(), reference.log.all.size())
            << "a different number of reports for request " << i;
        for (std::size_t r = reports_before; r < reference.log.all.size(); ++r) {
            ASSERT_EQ(candidate.log.all[r], reference.log.all[r])
                << "report " << r << ", request " << i;
        }
        ASSERT_EQ(candidate.messages.all.size(), reference.messages.all.size())
            << "a different number of market data messages for request " << i;
        for (std::size_t m = messages_before; m < reference.messages.all.size(); ++m) {
            ASSERT_EQ(candidate.messages.all[m], reference.messages.all[m])
                << "market data message " << m << ", request " << i;
        }
        ASSERT_EQ(candidate.engine->open_orders(), reference.engine->open_orders())
            << "after request " << i;
    }

    // The bytes a file would hold, and both books in full.
    EXPECT_EQ(candidate.writer.bytes(), reference.writer.bytes());
    EXPECT_EQ(candidate.engine->stats(), reference.engine->stats());
    for (std::uint32_t s = 1; s <= cfg.symbols; ++s) {
        const auto locate = static_cast<Locate>(s);
        for (const Side side : {Side::Buy, Side::Sell}) {
            EXPECT_EQ(test::orders_of(*candidate.engine, locate, side),
                      test::orders_of(*reference.engine, locate, side));
            EXPECT_EQ(test::levels_of(*candidate.engine, locate, side),
                      test::levels_of(*reference.engine, locate, side));
            EXPECT_EQ(candidate.engine->best(locate, side), reference.engine->best(locate, side));
        }
    }
}

TYPED_TEST(EngineDifferential, SameOutputOnABusyMarket) {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        expect_same_as_reference<TypeParam>(test::busy_config(seed), 10'000);
        ASSERT_FALSE(this->HasFatalFailure());
    }
}

TYPED_TEST(EngineDifferential, SameOutputOnAThinBook) {
    expect_same_as_reference<TypeParam>(
        {.seed = 31, .symbols = 2, .owners = 3, .target_live_orders = 4, .bad_per_million = 50'000},
        20'000);
}

TYPED_TEST(EngineDifferential, SameOutputWhenOneOwnerTradesWithItself) {
    expect_same_as_reference<TypeParam>(
        {.seed = 32, .symbols = 1, .owners = 1, .target_live_orders = 60}, 20'000);
}

TYPED_TEST(EngineDifferential, SameOutputOnADeepBookWithManySymbols) {
    expect_same_as_reference<TypeParam>({.seed = 33,
                                         .symbols = 200,
                                         .owners = 32,
                                         .target_live_orders = 20'000,
                                         .bad_per_million = 1'000},
                                        150'000);
}

TYPED_TEST(EngineDifferential, SameHashesOverALongRun) {
    // Too long to keep the output: both engines are reduced to the two hashes
    // flow_gen prints. Long enough that every order slot has been recycled
    // many times over, which is where a pool or an index goes wrong.
    const gen::FlowConfig cfg{.seed = 34, .symbols = 50, .target_live_orders = 3'000};
    constexpr std::uint64_t kCommands = 600'000;

    const auto hashes = []<class Impl>(std::type_identity<Impl>, const gen::FlowConfig& config,
                                       std::uint64_t commands) {
        using Reports = engine::TeeReports<gen::OrderFlow, engine::ReportHasher>;
        using Engine = typename Impl::template Engine<Reports, engine::ItchFeedWriter>;
        gen::OrderFlow flow(config);
        engine::ReportHasher report_hash;
        Reports reports(flow, report_hash);
        engine::ItchFeedWriter writer;
        const auto engine = std::make_unique<Engine>(reports, writer);
        engine::ByteHasher feed_hash;
        flow.open(*engine);
        for (std::uint64_t i = 0; i < commands; ++i) {
            gen::apply(*engine, flow.next());
            if (writer.bytes().size() > (std::size_t{1} << 20)) {
                feed_hash.update(writer.take());
            }
        }
        feed_hash.update(writer.take());
        return std::vector<std::uint64_t>{feed_hash.hash(), feed_hash.bytes(), report_hash.hash(),
                                          report_hash.reports(), engine->open_orders()};
    };
    EXPECT_EQ(hashes(std::type_identity<TypeParam>{}, cfg, kCommands),
              hashes(std::type_identity<engine::ReferenceEngineImpl>{}, cfg, kCommands));
}

}  // namespace
