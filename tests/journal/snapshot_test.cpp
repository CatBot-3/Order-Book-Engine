#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "obe/engine/engines.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/endian.hpp"
#include "obe/feed/messages.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/journal/replay.hpp"
#include "obe/journal/snapshot.hpp"
#include "obe/types.hpp"
#include "obe/util/crc32.hpp"
#include "support/engine_harness.hpp"
#include "support/flow_run.hpp"
#include "support/journal_run.hpp"

// Snapshots: saving what an engine holds, and loading it into a fresh one.
//
// The standard is the same as for replay: nothing that happens afterwards may
// tell the restored engine from the original. A snapshot can fail that in
// ways a replay cannot, because it has to carry everything explicitly: forget
// a counter, or the order of a queue, and the engine looks right and then
// answers the next request differently.
//
// These run on the reference engine, the one engine that is Restorable today.

namespace {

using namespace obe;
using journal::EngineState;
using journal::SnapshotInstrument;
using journal::SnapshotOrder;
using test::Recorded;
using test::same_state;

using Impl = engine::ReferenceEngineImpl;
using Replica = test::Replica<Impl>;

constexpr std::uint32_t kLocates = 64;

// A side byte that is neither buy nor sell.
// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
constexpr Side kNoSide = static_cast<Side>('Q');

// Capture, to bytes, back from bytes, into a fresh engine.
template <class Engine>
[[nodiscard]] ::testing::AssertionResult through_a_snapshot(const Engine& from, Replica& to,
                                                            std::uint64_t next_sequence = 1) {
    const EngineState state = journal::capture(from, next_sequence, kLocates);
    const std::vector<std::byte> file = journal::encode(state);
    const std::optional<EngineState> read = journal::decode(file);
    if (!read) {
        return ::testing::AssertionFailure() << "the snapshot did not decode";
    }
    if (!(*read == state)) {
        return ::testing::AssertionFailure() << "the snapshot decoded to something else";
    }
    if (!journal::restore(*read, *to.engine)) {
        return ::testing::AssertionFailure() << "the snapshot was refused";
    }
    return ::testing::AssertionSuccess();
}

// --- The claim -------------------------------------------------------------------

TEST(Snapshot, ARestoredEngineHoldsWhatTheOriginalHeld) {
    for (const gen::FlowConfig& cfg : test::property_configs()) {
        SCOPED_TRACE(test::describe(cfg));
        Recorded<Impl> original(cfg);
        for (int checkpoint = 0; checkpoint < 12; ++checkpoint) {
            original.run(170);
            Replica restored;
            ASSERT_TRUE(through_a_snapshot(*original.engine, restored));
            ASSERT_TRUE(same_state(*original.engine, *restored.engine));
            ASSERT_EQ(restored.engine->counters(), original.engine->counters());
            ASSERT_EQ(journal::state_digest(*restored.engine, kLocates),
                      journal::state_digest(*original.engine, kLocates));
            for (std::uint32_t i = 1; i <= cfg.symbols; ++i) {
                const auto locate = static_cast<Locate>(i);
                ASSERT_EQ(restored.engine->symbol(locate), original.engine->symbol(locate));
            }
            // A snapshot of the copy is the snapshot it was made from.
            ASSERT_EQ(journal::capture(*restored.engine, 1, kLocates),
                      journal::capture(*original.engine, 1, kLocates));
        }
    }
}

TEST(Snapshot, ARestoredEngineCannotBeToldApartAfterwards) {
    for (const gen::FlowConfig& cfg : test::property_configs()) {
        SCOPED_TRACE(test::describe(cfg));
        Recorded<Impl> original(cfg);
        original.run(1'500);
        Replica restored;
        ASSERT_TRUE(through_a_snapshot(*original.engine, restored));

        for (int i = 0; i < 1'500; ++i) {
            original.log.clear();
            original.messages.clear();
            restored.log.clear();
            restored.messages.clear();
            const gen::Command command = original.step();
            gen::apply(*restored.engine, command);
            ASSERT_EQ(restored.log.all, original.log.all) << "request " << i;
            ASSERT_EQ(restored.messages.all, original.messages.all) << "request " << i;
        }
        EXPECT_TRUE(same_state(*original.engine, *restored.engine));
    }
}

TEST(Snapshot, RestoringPublishesAndReportsNothing) {
    Recorded<Impl> original(test::busy_config(21));
    original.run(500);
    ASSERT_GT(original.engine->open_orders(), 0U);
    Replica restored;
    ASSERT_TRUE(through_a_snapshot(*original.engine, restored));
    EXPECT_TRUE(restored.log.all.empty());
    EXPECT_TRUE(restored.messages.all.empty());
}

// The way recovery really goes: the newest snapshot, then the journal records
// written since.
TEST(Snapshot, ASnapshotAndTheJournalSinceItRecoverTheEngine) {
    for (const gen::FlowConfig& cfg : test::property_configs()) {
        SCOPED_TRACE(test::describe(cfg));
        Recorded<Impl> original(cfg);
        original.run(1'000);
        const std::uint64_t at = original.writer.next_sequence();
        const std::vector<std::byte> snapshot =
            journal::encode(journal::capture(*original.engine, at, kLocates));
        original.run(700);  // trading goes on after the snapshot

        Replica recovered;
        const std::optional<EngineState> state = journal::decode(snapshot);
        ASSERT_TRUE(state.has_value());
        ASSERT_EQ(state->next_sequence, at);
        ASSERT_TRUE(journal::restore(*state, *recovered.engine));
        const journal::ReplayResult result =
            journal::replay(original.bytes, *recovered.engine, state->next_sequence);
        EXPECT_TRUE(result.clean());
        EXPECT_EQ(result.skipped, at - 1);
        EXPECT_EQ(result.applied, 700U);
        EXPECT_TRUE(same_state(*original.engine, *recovered.engine));
        EXPECT_EQ(recovered.engine->counters(), original.engine->counters());

        // What it said while catching up is the tail of what the original said.
        ASSERT_LE(recovered.log.all.size(), original.log.all.size());
        EXPECT_EQ(recovered.log.all, std::vector<test::Report>(
                                         original.log.all.end() -
                                             static_cast<std::ptrdiff_t>(recovered.log.all.size()),
                                         original.log.all.end()));
    }
}

// --- The parts a snapshot is easy to get wrong -------------------------------------

struct Small {
    Replica r;
    Nanos now = 1;

    Small() {
        r.engine->add_instrument(1, feed::Symbol::from("AAAA"), now++);
        r.engine->add_instrument(3, feed::Symbol::from("CCCC"), now++);
    }
    OrderId order(Locate locate, Side side, Price price, Qty qty, engine::OwnerId owner = 1,
                  engine::Token token = 0) {
        return r.engine->submit({.owner = owner,
                                 .token = token,
                                 .locate = locate,
                                 .side = side,
                                 .qty = qty,
                                 .price = price},
                                now++);
    }
};

TEST(Snapshot, ListsInstrumentsAndOrdersInTheOrderTheyWouldTrade) {
    Small s;
    const OrderId bid_low = s.order(1, Side::Buy, 9'900, 10, 1, 101);
    const OrderId bid_first = s.order(1, Side::Buy, 10'000, 20, 2, 102);
    const OrderId bid_second = s.order(1, Side::Buy, 10'000, 30, 3, 103);
    const OrderId ask_high = s.order(1, Side::Sell, 10'300, 40, 1, 104);
    const OrderId ask_best = s.order(1, Side::Sell, 10'100, 50, 2, 105);
    const OrderId other = s.order(3, Side::Sell, 500, 60, 4, 106);

    const EngineState state = journal::capture(*s.r.engine, 77, kLocates);
    EXPECT_EQ(state.next_sequence, 77U);
    EXPECT_EQ(state.counters, (engine::EngineCounters{.last_order_id = 6, .last_match_number = 0}));
    EXPECT_EQ(state.stats, s.r.engine->stats());
    EXPECT_EQ(state.instruments, (std::vector<SnapshotInstrument>{
                                     {1, feed::Symbol::from("AAAA")},
                                     {3, feed::Symbol::from("CCCC")},
                                 }));
    const std::vector<SnapshotOrder> expected{
        {1, Side::Buy, {bid_first, 2, 102, 10'000, 20}},
        {1, Side::Buy, {bid_second, 3, 103, 10'000, 30}},
        {1, Side::Buy, {bid_low, 1, 101, 9'900, 10}},
        {1, Side::Sell, {ask_best, 2, 105, 10'100, 50}},
        {1, Side::Sell, {ask_high, 1, 104, 10'300, 40}},
        {3, Side::Sell, {other, 4, 106, 500, 60}},
    };
    EXPECT_EQ(state.orders, expected);

    // It looks only as far as it is told to: locate 3 is beyond a bound of 2.
    const EngineState narrow = journal::capture(*s.r.engine, 1, 2);
    EXPECT_EQ(narrow.instruments.size(), 1U);
    EXPECT_EQ(narrow.orders.size(), 5U);
}

TEST(Snapshot, AnOrderKeepsItsPlaceInItsQueue) {
    Small s;
    s.order(1, Side::Buy, 10'000, 100, 1, 11);  // first in the queue
    s.order(1, Side::Buy, 10'000, 100, 2, 22);
    s.order(1, Side::Buy, 10'000, 100, 3, 33);
    Replica restored;
    ASSERT_TRUE(through_a_snapshot(*s.r.engine, restored));

    // A sale of 150 takes all of the first order and half of the second.
    restored.engine->submit(
        {.owner = 9, .token = 0, .locate = 1, .side = Side::Sell, .qty = 150, .price = 10'000}, 50);
    std::vector<engine::OwnerId> filled;
    for (const engine::Executed& e : restored.log.of<engine::Executed>()) {
        if (e.liquidity == engine::Liquidity::Added) {
            filled.push_back(e.owner);
        }
    }
    EXPECT_EQ(filled, (std::vector<engine::OwnerId>{1, 2}));
    const auto left = test::orders_of(*restored.engine, 1, Side::Buy);
    ASSERT_EQ(left.size(), 2U);
    EXPECT_EQ(left[0].owner, 2U);
    EXPECT_EQ(left[0].qty, 50U);
    EXPECT_EQ(left[1].owner, 3U);
}

// Neither number is in the book. An engine restored without them would give
// its next order an id that an order still resting already has.
TEST(Snapshot, IdsAndMatchNumbersCarryOnFromWhereTheyWere) {
    Small s;
    s.order(1, Side::Buy, 10'000, 100, 1);
    s.order(1, Side::Sell, 10'000, 40, 2);  // trades: match 1
    s.order(1, Side::Sell, 10'000, 10, 2);  // trades: match 2
    const OrderId last = s.order(1, Side::Sell, 10'500, 10, 2);
    ASSERT_EQ(last, 4U);
    ASSERT_EQ(s.r.engine->counters(),
              (engine::EngineCounters{.last_order_id = 4, .last_match_number = 2}));

    Replica restored;
    ASSERT_TRUE(through_a_snapshot(*s.r.engine, restored));
    const OrderId next = restored.engine->submit(
        {.owner = 5, .token = 0, .locate = 1, .side = Side::Sell, .qty = 20, .price = 10'000}, 60);
    EXPECT_EQ(next, 5U);
    const auto trades = restored.log.of<engine::Executed>();
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0].match_number, 3U);
    EXPECT_EQ(restored.engine->counters(),
              (engine::EngineCounters{.last_order_id = 5, .last_match_number = 3}));
}

TEST(Snapshot, TheTotalsCarryOnToo) {
    Recorded<Impl> original(test::busy_config(22));
    original.run(800);
    const engine::EngineStats stats = original.engine->stats();
    ASSERT_GT(stats.trades, 0U);
    ASSERT_GT(stats.rejected, 0U);
    Replica restored;
    ASSERT_TRUE(through_a_snapshot(*original.engine, restored));
    EXPECT_EQ(engine::EngineStats(restored.engine->stats()), stats);
}

// The digest is what two recoveries are compared by, so every total has to be
// in it. Through trading alone no total can be moved without moving another;
// restoring lets each be set by itself.
TEST(Snapshot, TheDigestCoversEachTotalSeparately) {
    Replica plain;
    const std::uint64_t base = journal::state_digest(*plain.engine, kLocates);
    std::uint64_t engine::EngineStats::*const totals[] = {
        &engine::EngineStats::accepted,        &engine::EngineStats::rejected,
        &engine::EngineStats::cancels,         &engine::EngineStats::replaces,
        &engine::EngineStats::trades,          &engine::EngineStats::traded_shares,
        &engine::EngineStats::unfilled_shares, &engine::EngineStats::self_matches};
    static_assert(sizeof(engine::EngineStats) == sizeof(totals), "a total is missing here");
    std::vector<std::uint64_t> seen{base};
    for (const auto total : totals) {
        Replica r;
        engine::EngineStats stats;
        stats.*total = 1;
        r.engine->restore_counters({}, stats);
        const std::uint64_t digest = journal::state_digest(*r.engine, kLocates);
        // Different from an engine with no totals, and from every other
        // total being 1: no two of them are folded in the same way.
        for (const std::uint64_t other : seen) {
            EXPECT_NE(digest, other);
        }
        seen.push_back(digest);
    }
}

TEST(Snapshot, AnEngineThatHasDoneNothingHasAnEmptySnapshotThatRestores) {
    Replica fresh;
    const EngineState state = journal::capture(*fresh.engine, 1, kLocates);
    EXPECT_EQ(state, EngineState{});
    EXPECT_TRUE(journal::plausible(state));
    const std::vector<std::byte> file = journal::encode(state);
    EXPECT_EQ(file.size(), journal::kSnapshotFixedSize + 4);
    Replica restored;
    ASSERT_TRUE(journal::restore(*journal::decode(file), *restored.engine));
    EXPECT_TRUE(same_state(*fresh.engine, *restored.engine));
}

// --- The engine's side of it -------------------------------------------------------

TEST(Restorable, AnInstrumentCanBeOpenedSilentlyOnce) {
    Replica r;
    EXPECT_TRUE(r.engine->restore_instrument(4, feed::Symbol::from("DDDD")));
    EXPECT_TRUE(r.engine->listed(4));
    EXPECT_EQ(r.engine->symbol(4), feed::Symbol::from("DDDD"));
    EXPECT_FALSE(r.engine->restore_instrument(4, feed::Symbol::from("EEEE")));
    EXPECT_EQ(r.engine->symbol(4), feed::Symbol::from("DDDD"));
    EXPECT_TRUE(r.messages.all.empty());
    // And an instrument opened the ordinary way cannot be restored over.
    r.engine->add_instrument(5, feed::Symbol::from("FFFF"), 1);
    EXPECT_FALSE(r.engine->restore_instrument(5, feed::Symbol::from("GGGG")));
}

TEST(Restorable, AnOrderThatCouldNotBeRestingIsRefusedAndChangesNothing) {
    Replica r;
    ASSERT_TRUE(r.engine->restore_instrument(1, feed::Symbol::from("AAAA")));
    const engine::RestingOrder good{.id = 7, .owner = 2, .token = 3, .price = 10'000, .qty = 100};
    ASSERT_TRUE(r.engine->restore_order(1, Side::Buy, good));

    engine::RestingOrder no_shares = good;
    no_shares.id = 8;
    no_shares.qty = 0;
    engine::RestingOrder no_price = good;
    no_price.id = 9;
    no_price.price = 0;
    engine::RestingOrder fine = good;
    fine.id = 10;
    EXPECT_FALSE(r.engine->restore_order(2, Side::Buy, fine)) << "instrument not open";
    EXPECT_FALSE(r.engine->restore_order(1, kNoSide, fine)) << "not a side";
    EXPECT_FALSE(r.engine->restore_order(1, Side::Buy, no_shares));
    EXPECT_FALSE(r.engine->restore_order(1, Side::Buy, no_price));
    EXPECT_FALSE(r.engine->restore_order(1, Side::Sell, good)) << "the id is already resting";

    EXPECT_EQ(r.engine->open_orders(), 1U);
    EXPECT_EQ(test::orders_of(*r.engine, 1, Side::Buy), (std::vector<engine::RestingOrder>{good}));
    EXPECT_TRUE(test::orders_of(*r.engine, 1, Side::Sell).empty());
    EXPECT_TRUE(test::orders_of(*r.engine, 2, Side::Buy).empty());
    EXPECT_TRUE(r.log.all.empty());
    EXPECT_TRUE(r.messages.all.empty());
}

TEST(Restorable, RestoredOrdersQueueInTheOrderGivenAndCanBeCancelledByTheirOwners) {
    Replica r;
    ASSERT_TRUE(r.engine->restore_instrument(1, feed::Symbol::from("AAAA")));
    ASSERT_TRUE(r.engine->restore_order(1, Side::Sell, {40, 1, 0, 10'000, 10}));
    ASSERT_TRUE(r.engine->restore_order(1, Side::Sell, {12, 2, 0, 10'000, 20}));
    ASSERT_TRUE(r.engine->restore_order(1, Side::Sell, {30, 3, 0, 9'900, 30}));
    r.engine->restore_counters({.last_order_id = 40, .last_match_number = 0}, {});

    const auto asks = test::orders_of(*r.engine, 1, Side::Sell);
    ASSERT_EQ(asks.size(), 3U);
    EXPECT_EQ(asks[0].id, 30U);  // the better price
    EXPECT_EQ(asks[1].id, 40U);  // then the queue at 10'000, as it was given
    EXPECT_EQ(asks[2].id, 12U);
    EXPECT_EQ(r.engine->best(1, Side::Sell), (book::Level{9'900, 30}));
    const auto levels = test::levels_of(*r.engine, 1, Side::Sell);
    ASSERT_EQ(levels.size(), 2U);
    EXPECT_EQ(levels[1], (book::Level{10'000, 30}));

    // The index was rebuilt as well: the orders can be found by id.
    EXPECT_FALSE(r.engine->cancel(1, 12, 5)) << "not owner 1's";
    EXPECT_TRUE(r.engine->cancel(2, 12, 6));
    EXPECT_EQ(r.engine->replace(1, 40, 5, 10'000, 7), 40U);
    EXPECT_EQ(r.engine->open_orders(), 2U);
}

// A restored iceberg's reserve has to be counted where a fill-or-kill counts
// it. Listed, the order looks right whether the engine added its hidden shares
// to the level's reserve or not; only an order that needs those shares shows
// which.
TEST(Restorable, ARestoredReserveIsThereForAFillOrKill) {
    Replica r;
    ASSERT_TRUE(r.engine->restore_instrument(1, feed::Symbol::from("AAAA")));
    ASSERT_TRUE(r.engine->restore_order(1, Side::Sell,
                                        {.id = 5,
                                         .owner = 1,
                                         .token = 0,
                                         .price = 10'000,
                                         .qty = 100,
                                         .hidden = 400,
                                         .display = 100}));
    r.engine->restore_counters({.last_order_id = 5, .last_match_number = 0}, {});
    const auto fill_or_kill = [&r](Qty qty) {
        r.engine->submit({.owner = 2,
                          .token = 1,
                          .locate = 1,
                          .side = Side::Buy,
                          .qty = qty,
                          .price = 10'000,
                          .tif = engine::TimeInForce::FillOrKill},
                         9);
    };
    fill_or_kill(501);  // one more than is there
    EXPECT_EQ(r.engine->stats().traded_shares, 0U);
    EXPECT_EQ(r.engine->open_orders(), 1U);
    fill_or_kill(500);  // all of it, four fifths of which are hidden
    EXPECT_EQ(r.engine->stats().traded_shares, 500U) << "the hidden shares were not counted";
    EXPECT_EQ(r.engine->open_orders(), 0U);
}

// --- restore() ---------------------------------------------------------------------

// A state that is right in every way, to spoil one thing at a time.
EngineState good_state() {
    EngineState s;
    s.next_sequence = 10;
    s.counters = {.last_order_id = 9, .last_match_number = 4};
    s.stats = {.accepted = 9,
               .rejected = 1,
               .cancels = 2,
               .replaces = 1,
               .trades = 4,
               .traded_shares = 400,
               .unfilled_shares = 30,
               .self_matches = 6};
    s.instruments = {{1, feed::Symbol::from("AAAA")}, {2, feed::Symbol::from("BBBB")}};
    s.orders = {
        {1, Side::Buy, {3, 1, 11, 10'000, 100}},
        {1, Side::Buy, {5, 2, 12, 9'900, 200}},
        // An iceberg on its second slice (the market knows it as 8), which is
        // also post-only and cancels its owner's resting orders on meeting them.
        {1,
         Side::Sell,
         {.id = 4,
          .owner = 1,
          .token = 13,
          .price = 10'100,
          .qty = 300,
          .hidden = 700,
          .display = 350,
          .ref = 8,
          .post_only = true,
          .self_match = engine::SelfMatch::CancelResting}},
        {2, Side::Sell, {9, 3, 14, 500, 400}},
    };
    return s;
}

TEST(Snapshot, AGoodStateRestores) {
    const EngineState state = good_state();
    ASSERT_TRUE(journal::plausible(state));
    Replica r;
    ASSERT_TRUE(journal::restore(state, *r.engine));
    EXPECT_EQ(journal::capture(*r.engine, 10, kLocates), state);
}

TEST(Snapshot, AStateNoEngineCouldHoldIsRefusedBeforeAnythingIsTouched) {
    struct Case {
        const char* what;
        void (*spoil)(EngineState&);
    };
    const Case cases[] = {
        {"an instrument listed twice",
         [](EngineState& s) { s.instruments.push_back(s.instruments[0]); }},
        {"an order for an instrument that is not open",
         [](EngineState& s) { s.orders[0].locate = 7; }},
        {"an order on no side", [](EngineState& s) { s.orders[0].side = kNoSide; }},
        {"an order for no shares", [](EngineState& s) { s.orders[1].order.qty = 0; }},
        {"an order with no price", [](EngineState& s) { s.orders[1].order.price = 0; }},
        {"an order with no id", [](EngineState& s) { s.orders[2].order.id = 0; }},
        {"an id used twice, even across instruments",
         [](EngineState& s) { s.orders[3].order.id = s.orders[0].order.id; }},
        {"an id the counter has not reached", [](EngineState& s) { s.orders[2].order.id = 10; }},
        {"a counter behind the ids", [](EngineState& s) { s.counters.last_order_id = 8; }},
        {"a crossed book", [](EngineState& s) { s.orders[2].order.price = 9'950; }},
        {"a locked book", [](EngineState& s) { s.orders[2].order.price = 10'000; }},
        {"a book crossed by a bid that is not the first listed",
         [](EngineState& s) { s.orders[1].order.price = 10'200; }},
        {"hidden shares on an order with no display size",
         [](EngineState& s) { s.orders[0].order.hidden = 5; }},
        {"an iceberg showing more than its display size while it still hides some",
         [](EngineState& s) { s.orders[2].order.qty = 351; }},
        {"more open shares than an order can be entered for",
         [](EngineState& s) {
             s.orders[2].order.display = ~Qty{0};
             s.orders[2].order.qty = ~Qty{0};
             s.orders[2].order.hidden = 1;
         }},
        {"a market reference that is another order's id",
         [](EngineState& s) { s.orders[2].order.ref = 9; }},
        {"a market reference the counter has not reached",
         [](EngineState& s) { s.orders[2].order.ref = 10; }},
        {"two orders known to the market by one reference",
         [](EngineState& s) { s.orders[0].order.ref = 8; }},
        {"a market reference that only repeats the order's id",
         [](EngineState& s) { s.orders[0].order.ref = 3; }},
        {"a self-match instruction no engine keeps",
         [](EngineState& s) {
             // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
             s.orders[0].order.self_match = static_cast<engine::SelfMatch>(4);
         }},
        {"a book crossed by an ask that is not the last listed",
         [](EngineState& s) {
             s.orders.push_back({1, Side::Sell, {6, 1, 15, 10'300, 10}});
             s.orders[0].order.price = 10'200;  // above the ask at 10'100, below this one
         }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        EngineState state = good_state();
        c.spoil(state);
        EXPECT_FALSE(journal::plausible(state));
        Replica r;
        EXPECT_FALSE(journal::restore(state, *r.engine));
        EXPECT_EQ(r.engine->open_orders(), 0U);
        EXPECT_FALSE(r.engine->listed(1));
        EXPECT_FALSE(r.engine->listed(2));
        EXPECT_EQ(r.engine->counters(), engine::EngineCounters{});
        // The engine is untouched, so the right state can still go into it.
        EXPECT_TRUE(journal::restore(good_state(), *r.engine));
    }
}

TEST(Snapshot, ThingsThatLookOddButAreNotWrongAreAccepted) {
    {
        SCOPED_TRACE("an id at the counter exactly");
        EngineState s = good_state();
        s.orders[2].order.id = 9;
        s.orders[3].order.id = 4;
        EXPECT_TRUE(journal::plausible(s));
    }
    {
        SCOPED_TRACE("a bid on one instrument above an ask on another");
        EngineState s = good_state();
        s.orders[1].order.price = 600;  // locate 1 bid; locate 2's ask is 500
        EXPECT_TRUE(journal::plausible(s));
    }
    {
        SCOPED_TRACE("a book one tick wide");
        EngineState s = good_state();
        s.orders[2].order.price = 10'001;
        EXPECT_TRUE(journal::plausible(s));
    }
    {
        SCOPED_TRACE("an iceberg down to its last slice: a display size and nothing hidden");
        EngineState s = good_state();
        s.orders[2].order.hidden = 0;
        EXPECT_TRUE(journal::plausible(s));
    }
    {
        SCOPED_TRACE("an iceberg showing exactly its display size");
        EngineState s = good_state();
        s.orders[2].order.qty = 350;
        EXPECT_TRUE(journal::plausible(s));
    }
    {
        SCOPED_TRACE("a market reference at the counter exactly");
        EngineState s = good_state();
        s.counters.last_order_id = 10;
        s.orders[2].order.ref = 10;
        EXPECT_TRUE(journal::plausible(s));
    }
    {
        SCOPED_TRACE("an instrument with no orders");
        EngineState s = good_state();
        s.instruments.push_back({40'000, feed::Symbol::from("FARR")});
        EXPECT_TRUE(journal::plausible(s));
    }
}

// Each case fails exactly one of the checks, and uses a locate the state does
// not, so that nothing else can be what refuses it.
TEST(Snapshot, OnlyAnEngineThatHasDoneNothingCanBeRestoredInto) {
    const auto refused_and_untouched = [](Replica& r) {
        const std::uint64_t before = journal::state_digest(*r.engine, kLocates);
        EXPECT_FALSE(journal::restore(good_state(), *r.engine));
        EXPECT_EQ(journal::state_digest(*r.engine, kLocates), before);
        EXPECT_FALSE(r.engine->listed(1));
    };
    {
        SCOPED_TRACE("it has traded: an order is resting");
        Replica r;
        r.engine->add_instrument(9, feed::Symbol::from("NINE"), 1);
        r.engine->submit(
            {.owner = 1, .token = 0, .locate = 9, .side = Side::Buy, .qty = 5, .price = 100}, 2);
        refused_and_untouched(r);
    }
    {
        SCOPED_TRACE("it holds an order and has given out no id: a restore left half done");
        Replica r;
        ASSERT_TRUE(r.engine->restore_instrument(9, feed::Symbol::from("NINE")));
        ASSERT_TRUE(r.engine->restore_order(9, Side::Buy, {1, 1, 0, 100, 5}));
        ASSERT_EQ(r.engine->counters(), engine::EngineCounters{});
        refused_and_untouched(r);
    }
    {
        SCOPED_TRACE("it has no orders left, but has given out ids");
        Replica r;
        r.engine->add_instrument(9, feed::Symbol::from("NINE"), 1);
        const OrderId id = r.engine->submit(
            {.owner = 1, .token = 0, .locate = 9, .side = Side::Buy, .qty = 5, .price = 100}, 2);
        ASSERT_TRUE(r.engine->cancel(1, id, 3));
        ASSERT_EQ(r.engine->open_orders(), 0U);
        refused_and_untouched(r);
    }
    {
        SCOPED_TRACE("it has one of the state's instruments open");
        Replica r;
        r.engine->add_instrument(2, feed::Symbol::from("ZZZZ"), 1);
        EXPECT_FALSE(journal::restore(good_state(), *r.engine));
        EXPECT_EQ(r.engine->symbol(2), feed::Symbol::from("ZZZZ"));
        EXPECT_EQ(r.engine->open_orders(), 0U);
    }
}

// --- The file ----------------------------------------------------------------------

std::uint64_t u64_at(const std::vector<std::byte>& b, std::size_t at) {
    return feed::load_be<std::uint64_t>(b.data() + at);
}
std::uint32_t u32_at(const std::vector<std::byte>& b, std::size_t at) {
    return feed::load_be<std::uint32_t>(b.data() + at);
}
std::uint16_t u16_at(const std::vector<std::byte>& b, std::size_t at) {
    return feed::load_be<std::uint16_t>(b.data() + at);
}

// Puts the checksum right after the bytes have been changed on purpose, so
// that a test reaches the check it means to and not the checksum.
void reseal(std::vector<std::byte>& file) {
    feed::store_be<std::uint32_t>(file.data() + file.size() - 4,
                                  util::crc32({file.data(), file.size() - 4}));
}

TEST(SnapshotFile, IsLaidOutAsDocumented) {
    const EngineState state = good_state();
    const std::vector<std::byte> file = journal::encode(state);

    ASSERT_EQ(file.size(), 104U + 2 * 10 + 4 * 49 + 4);
    EXPECT_EQ(static_cast<char>(file[0]), 'O');
    EXPECT_EQ(static_cast<char>(file[1]), 'B');
    EXPECT_EQ(static_cast<char>(file[2]), 'E');
    EXPECT_EQ(static_cast<char>(file[3]), 'S');
    EXPECT_EQ(u16_at(file, 4), 2U);  // version
    EXPECT_EQ(u16_at(file, 6), 0U);
    EXPECT_EQ(u64_at(file, 8), 10U);  // next journal sequence
    EXPECT_EQ(u64_at(file, 16), 9U);  // last order id
    EXPECT_EQ(u64_at(file, 24), 4U);  // last match number
    const std::uint64_t stats[8] = {9, 1, 2, 1, 4, 400, 30, 6};
    for (std::size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(u64_at(file, 32 + 8 * i), stats[i]) << i;
    }
    EXPECT_EQ(u32_at(file, 96), 2U);
    EXPECT_EQ(u32_at(file, 100), 4U);
    // The second instrument.
    EXPECT_EQ(u16_at(file, 114), 2U);
    EXPECT_EQ(static_cast<char>(file[116]), 'B');
    EXPECT_EQ(static_cast<char>(file[123]), ' ');
    // The third order, the one with something in every field: locate 1, sell,
    // id 4, owner 1, token 13, 300 showing at 10'100, 700 hidden, display 350,
    // known to the market as 8, post-only, self-match mode 2.
    const std::size_t at = 104 + 20 + 2 * 49;
    EXPECT_EQ(u16_at(file, at), 1U);
    EXPECT_EQ(static_cast<char>(file[at + 2]), 'S');
    EXPECT_EQ(u64_at(file, at + 3), 4U);
    EXPECT_EQ(u32_at(file, at + 11), 1U);
    EXPECT_EQ(u64_at(file, at + 15), 13U);
    EXPECT_EQ(u32_at(file, at + 23), 10'100U);
    EXPECT_EQ(u32_at(file, at + 27), 300U);
    EXPECT_EQ(u32_at(file, at + 31), 700U);
    EXPECT_EQ(u32_at(file, at + 35), 350U);
    EXPECT_EQ(u64_at(file, at + 39), 8U);
    EXPECT_EQ(file[at + 47], std::byte{1});
    EXPECT_EQ(file[at + 48], std::byte{2});
    // A plain order writes zeros in all five: the order before it.
    const std::size_t plain = at - 49;
    EXPECT_EQ(u32_at(file, plain + 31), 0U);
    EXPECT_EQ(u32_at(file, plain + 35), 0U);
    EXPECT_EQ(u64_at(file, plain + 39), 0U);
    EXPECT_EQ(file[plain + 47], std::byte{0});
    EXPECT_EQ(file[plain + 48], std::byte{0});
    // And the checksum of all of it at the end.
    EXPECT_EQ(u32_at(file, file.size() - 4), util::crc32({file.data(), file.size() - 4}));
}

// The post-only byte is written as 0 or 1. A file with anything else there
// was not written by this code, and reading it as "true" would let two
// different files decode to the same state.
TEST(SnapshotFile, AFlagByteThatIsNeitherZeroNorOneIsRefused) {
    std::vector<std::byte> file = journal::encode(good_state());
    const std::size_t flag = 104 + 20 + 2 * 49 + 47;
    ASSERT_EQ(file[flag], std::byte{1});
    file[flag] = std::byte{2};
    reseal(file);
    EXPECT_FALSE(journal::decode(file).has_value());
    file[flag] = std::byte{0};
    reseal(file);
    EXPECT_TRUE(journal::decode(file).has_value());
}

TEST(SnapshotFile, SurvivesTheRoundTripWithEveryFieldAtItsLargest) {
    EngineState state;
    state.next_sequence = ~std::uint64_t{0};
    state.counters = {.last_order_id = ~std::uint64_t{0} - 1,
                      .last_match_number = ~std::uint64_t{0} - 2};
    state.stats = {.accepted = ~std::uint64_t{0} - 3,
                   .rejected = ~std::uint64_t{0} - 4,
                   .cancels = ~std::uint64_t{0} - 5,
                   .replaces = ~std::uint64_t{0} - 6,
                   .trades = ~std::uint64_t{0} - 7,
                   .traded_shares = ~std::uint64_t{0} - 8,
                   .unfilled_shares = ~std::uint64_t{0} - 9,
                   .self_matches = ~std::uint64_t{0} - 15};
    state.instruments = {{65'535, feed::Symbol::from("ZZZZZZZZ")}};
    state.orders = {{65'535,
                     Side::Sell,
                     {.id = ~std::uint64_t{0} - 10,
                      .owner = ~std::uint32_t{0} - 11,
                      .token = ~std::uint64_t{0} - 12,
                      .price = ~std::uint32_t{0} - 13,
                      .qty = ~std::uint32_t{0} - 14,
                      .hidden = ~std::uint32_t{0} - 16,
                      .display = ~std::uint32_t{0} - 17,
                      .ref = ~std::uint64_t{0} - 18,
                      .post_only = true,
                      // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                      .self_match = static_cast<engine::SelfMatch>(255)}}};
    const std::optional<EngineState> back = journal::decode(journal::encode(state));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, state);
}

TEST(SnapshotFile, ASnapshotCutShortAnywhereIsNotASnapshot) {
    const std::vector<std::byte> file = journal::encode(good_state());
    for (std::size_t n = 0; n < file.size(); ++n) {
        // A copy of exactly n bytes, so that a read beyond them is a read
        // outside an allocation and AddressSanitizer sees it.
        const std::vector<std::byte> cut(file.begin(),
                                         file.begin() + static_cast<std::ptrdiff_t>(n));
        ASSERT_FALSE(journal::decode(cut).has_value()) << n;
    }
    std::vector<std::byte> longer = file;
    longer.push_back(std::byte{0});
    EXPECT_FALSE(journal::decode(longer).has_value());
    EXPECT_TRUE(journal::decode(file).has_value());
}

TEST(SnapshotFile, NoSingleBitErrorGetsThrough) {
    std::vector<std::byte> file = journal::encode(good_state());
    for (std::size_t i = 0; i < file.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            file[i] ^= static_cast<std::byte>(1 << bit);
            ASSERT_FALSE(journal::decode(file).has_value()) << "byte " << i << " bit " << bit;
            file[i] ^= static_cast<std::byte>(1 << bit);
        }
    }
}

// The checks other than the checksum, reached by fixing the checksum up.
TEST(SnapshotFile, AWholeFileOfTheWrongKindIsRefused) {
    const std::vector<std::byte> good = journal::encode(good_state());
    {
        SCOPED_TRACE("a journal's magic");
        std::vector<std::byte> file = good;
        file[3] = std::byte{'J'};
        reseal(file);
        EXPECT_FALSE(journal::decode(file).has_value());
    }
    {
        SCOPED_TRACE("a later version");
        std::vector<std::byte> file = good;
        ASSERT_EQ(u16_at(file, 4), 2U) << "the current version";
        feed::store_be<std::uint16_t>(file.data() + 4, 3);
        reseal(file);
        EXPECT_FALSE(journal::decode(file).has_value());
    }
    {
        SCOPED_TRACE("the version before, whose orders were shorter");
        std::vector<std::byte> file = good;
        feed::store_be<std::uint16_t>(file.data() + 4, 1);
        reseal(file);
        EXPECT_FALSE(journal::decode(file).has_value());
    }
    {
        SCOPED_TRACE("something in the field that must be zero");
        std::vector<std::byte> file = good;
        feed::store_be<std::uint16_t>(file.data() + 6, 1);
        reseal(file);
        EXPECT_FALSE(journal::decode(file).has_value());
    }
    {
        SCOPED_TRACE("resealing alone changes nothing");
        std::vector<std::byte> file = good;
        reseal(file);
        EXPECT_EQ(file, good);
    }
}

TEST(SnapshotFile, CountsThatDoNotMatchTheLengthAreRefusedWithoutBeingBelieved) {
    const std::vector<std::byte> good = journal::encode(good_state());
    struct Counts {
        std::uint32_t instruments;
        std::uint32_t orders;
    };
    // The good file has 2 and 4. The last four would overflow a 32-bit
    // product, or ask for gigabytes, if the counts were used before being
    // checked against the length.
    const Counts wrong[] = {{2, 5},
                            {2, 3},
                            {3, 4},
                            {1, 4},
                            {0, 0},
                            {0xFFFFFFFFU, 4},
                            {2, 0xFFFFFFFFU},
                            {0xFFFFFFFFU, 0xFFFFFFFFU},
                            {0x80000000U, 0x80000000U}};
    for (const Counts& c : wrong) {
        SCOPED_TRACE(::testing::Message() << c.instruments << " and " << c.orders);
        std::vector<std::byte> file = good;
        feed::store_be<std::uint32_t>(file.data() + 96, c.instruments);
        feed::store_be<std::uint32_t>(file.data() + 100, c.orders);
        reseal(file);
        EXPECT_FALSE(journal::decode(file).has_value());
    }
}

// A file can be whole and still describe nothing an engine could hold. The
// file check passes it and the state check stops it.
TEST(SnapshotFile, AnIntactFileHoldingNonsenseDecodesAndIsThenRefused) {
    EngineState state = good_state();
    state.orders[0].side = kNoSide;
    const std::optional<EngineState> back = journal::decode(journal::encode(state));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, state);
    EXPECT_FALSE(journal::plausible(*back));
    Replica r;
    EXPECT_FALSE(journal::restore(*back, *r.engine));
}

TEST(SnapshotFile, SizesGrowByAFixedAmountPerInstrumentAndPerOrder) {
    Recorded<Impl> original(test::busy_config(23));
    original.run(400);
    const EngineState state = journal::capture(*original.engine, 1, kLocates);
    ASSERT_GT(state.orders.size(), 10U);
    EXPECT_EQ(state.orders.size(), original.engine->open_orders());
    EXPECT_EQ(journal::encode(state).size(),
              journal::kSnapshotFixedSize +
                  state.instruments.size() * journal::kSnapshotInstrumentSize +
                  state.orders.size() * journal::kSnapshotOrderSize + 4);
}

}  // namespace
