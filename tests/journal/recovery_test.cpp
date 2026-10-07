#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "obe/engine/concepts.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/journal/reader.hpp"
#include "obe/journal/records.hpp"
#include "obe/journal/replay.hpp"
#include "obe/journal/writer.hpp"
#include "obe/types.hpp"
#include "support/engine_harness.hpp"
#include "support/flow_run.hpp"
#include "support/journal_run.hpp"

// Recovery by replay.
//
// The claim under test is one sentence: an engine that has been given a
// journal's records is the engine that wrote them. Everything here is a way of
// trying to tell the two apart: by what they hold, by what they said while the
// journal was replayed, and by what they say to every request afterwards.
//
// Replay needs nothing from an engine beyond the ordinary contract, so this
// suite is typed over the engines and runs on the hand-written one too.

namespace {

using namespace obe;
using journal::ReadStatus;
using journal::ReplayResult;
using test::AppendTo;
using test::MemoryWriter;
using test::Recorded;
using test::Replica;
using test::same_state;

constexpr std::uint32_t kLocates = 64;

template <class Engine>
std::uint64_t digest(const Engine& engine) {
    return journal::state_digest(engine, kLocates);
}

std::span<const std::byte> first(const std::vector<std::byte>& bytes, std::size_t n) {
    return {bytes.data(), n};
}

template <class Impl>
class Recovery : public ::testing::Test {};
TYPED_TEST_SUITE(Recovery, test::EngineTypes);

// --- The claim -------------------------------------------------------------------

TYPED_TEST(Recovery, ReplayRebuildsTheEngineAndEverythingItSaid) {
    for (const gen::FlowConfig& cfg : test::property_configs()) {
        SCOPED_TRACE(test::describe(cfg));
        Recorded<TypeParam> original(cfg);
        original.run(3'000);

        Replica<TypeParam> recovered;
        const ReplayResult result = journal::replay(original.bytes, *recovered.engine);

        EXPECT_EQ(result.status, ReadStatus::End);
        EXPECT_TRUE(result.clean());
        EXPECT_EQ(result.applied, original.writer.records());
        EXPECT_EQ(result.skipped, 0U);
        EXPECT_EQ(result.good_bytes, original.bytes.size());
        EXPECT_EQ(result.next_sequence, original.writer.next_sequence());

        EXPECT_TRUE(same_state(*original.engine, *recovered.engine));
        EXPECT_EQ(digest(*recovered.engine), digest(*original.engine));
        // Not only where it ended up: every report and every market-data
        // message along the way, in order.
        EXPECT_EQ(recovered.log.all, original.log.all);
        EXPECT_EQ(recovered.messages.all, original.messages.all);
    }
}

TYPED_TEST(Recovery, ARecoveredEngineCannotBeToldApartAfterwards) {
    for (const gen::FlowConfig& cfg : test::property_configs()) {
        SCOPED_TRACE(test::describe(cfg));
        Recorded<TypeParam> original(cfg);
        original.run(1'500);

        Replica<TypeParam> recovered;
        ASSERT_TRUE(journal::replay(original.bytes, *recovered.engine).clean());

        // From here both are given the same requests. The ids the recovered
        // engine hands out, its match numbers and the queue positions of the
        // orders it was rebuilt with all show in what it says.
        for (int i = 0; i < 1'500; ++i) {
            original.log.clear();
            original.messages.clear();
            recovered.log.clear();
            recovered.messages.clear();
            const gen::Command command = original.step();
            gen::apply(*recovered.engine, command);
            ASSERT_EQ(recovered.log.all, original.log.all) << "request " << i;
            ASSERT_EQ(recovered.messages.all, original.messages.all) << "request " << i;
        }
        EXPECT_TRUE(same_state(*original.engine, *recovered.engine));
    }
}

// --- Recovering from less than the whole journal ----------------------------------

TYPED_TEST(Recovery, StoppingAfterAnyRecordGivesTheEngineAsItWasThen) {
    Recorded<TypeParam> original(test::thin_config(7));
    original.run(150);
    const std::vector<std::size_t> ends = original.ends();
    ASSERT_EQ(ends.size(), original.writer.records() + 1);

    for (std::uint64_t records = 0; records < ends.size(); ++records) {
        Replica<TypeParam> recovered;
        const ReplayResult result =
            journal::replay(first(original.bytes, ends[records]), *recovered.engine);
        ASSERT_EQ(result.status, ReadStatus::End) << records;
        ASSERT_EQ(result.applied, records);
        ASSERT_EQ(result.next_sequence, records + 1);
        if (records >= original.opened) {
            ASSERT_EQ(digest(*recovered.engine), original.digests[records])
                << "after " << records << " records";
        }
    }
}

// What a crash leaves: the journal stops somewhere inside a record. Wherever
// that is, recovery must stop at the last whole record, say so, and say where
// the file should be cut.
//
// JournalReader.AJournalCutOffAnywhereIsATornTailAtTheLastWholeRecord tries
// every byte against the reader alone. Here an engine is rebuilt for each cut,
// which costs a few milliseconds, so the cuts are chosen: every byte of the
// file header and the first record, and around and inside every later one.
TYPED_TEST(Recovery, AJournalCutInsideAnyRecordRecoversToTheLastWholeRecord) {
    Recorded<TypeParam> original(test::thin_config(8));
    original.run(60);
    const std::vector<std::size_t> ends = original.ends();

    std::vector<std::size_t> cuts;
    for (std::size_t cut = 0; cut <= ends[1]; ++cut) {
        cuts.push_back(cut);
    }
    for (std::size_t r = 1; r + 1 < ends.size(); ++r) {
        const std::size_t start = ends[r];
        const std::size_t length = ends[r + 1] - start;
        for (const std::size_t into :
             {std::size_t{1}, std::size_t{4}, std::size_t{6}, journal::kRecordHeaderSize,
              journal::kRecordHeaderSize + 1, length / 2, length - 1, length}) {
            cuts.push_back(start + into);
        }
    }

    for (const std::size_t cut : cuts) {
        std::uint64_t whole = 0;  // records that fit in the first `cut` bytes
        while (whole + 1 < ends.size() && ends[whole + 1] <= cut) {
            ++whole;
        }
        Replica<TypeParam> recovered;
        const ReplayResult result = journal::replay(first(original.bytes, cut), *recovered.engine);

        ASSERT_TRUE(result.usable()) << "cut at " << cut;
        if (cut < journal::kFileHeaderSize) {
            // Not even the header: an empty journal, to be started afresh.
            ASSERT_EQ(result.status, ReadStatus::TornTail) << cut;
            ASSERT_EQ(result.good_bytes, 0U) << cut;
            ASSERT_EQ(result.applied, 0U) << cut;
            ASSERT_EQ(result.next_sequence, 1U) << cut;
            ASSERT_FALSE(recovered.engine->listed(1));
            continue;
        }
        const bool at_a_boundary = cut == ends[whole];
        ASSERT_EQ(result.status, at_a_boundary ? ReadStatus::End : ReadStatus::TornTail) << cut;
        ASSERT_EQ(result.clean(), at_a_boundary) << cut;
        ASSERT_EQ(result.applied, whole) << cut;
        ASSERT_EQ(result.good_bytes, ends[whole]) << cut;
        ASSERT_EQ(result.next_sequence, whole + 1) << cut;
        if (whole >= original.opened) {
            ASSERT_EQ(digest(*recovered.engine), original.digests[whole]) << "cut at " << cut;
        }
    }
}

// The whole cycle: crash, recover, cut the file, carry on writing to it. The
// journal that results must be the one that would have been written had there
// been no crash. That is checked byte for byte against a twin that never
// crashed and was given the same requests.
TYPED_TEST(Recovery, AJournalCarriedOnAfterACrashIsTheOneThatNeverCrashed) {
    const gen::FlowConfig cfg = test::busy_config(9);
    constexpr std::uint64_t kBefore = 500;
    constexpr std::uint64_t kLost = 3;  // requests the crash took with it
    constexpr std::uint64_t kAfter = 500;

    // The run that crashes. Its last kLost records never reach the journal
    // whole: the file ends in the middle of the first of them.
    Recorded<TypeParam> crashed(cfg);
    crashed.run(kBefore + kLost);
    const std::vector<std::size_t> ends = crashed.ends();
    const std::uint64_t kept = crashed.opened + kBefore;
    std::vector<std::byte> file(
        crashed.bytes.begin(), crashed.bytes.begin() + static_cast<std::ptrdiff_t>(ends[kept] + 9));

    // Recovery.
    Replica<TypeParam> recovered;
    const ReplayResult result = journal::replay(file, *recovered.engine);
    ASSERT_EQ(result.status, ReadStatus::TornTail);
    ASSERT_EQ(result.applied, kept);
    ASSERT_EQ(result.good_bytes, ends[kept]);
    file.resize(result.good_bytes);
    MemoryWriter writer(AppendTo{&file}, result.next_sequence, /*write_header=*/false);
    journal::Journaled journaled(*recovered.engine, writer);

    // The twin: same seed, so the same requests, and no crash. It supplies
    // the requests that come after, since its generator knows what is resting.
    Recorded<TypeParam> twin(cfg);
    twin.run(kBefore);
    ASSERT_TRUE(same_state(*twin.engine, *recovered.engine));
    for (std::uint64_t i = 0; i < kAfter; ++i) {
        gen::apply(journaled, twin.step());
    }

    EXPECT_EQ(file, twin.bytes);
    EXPECT_TRUE(same_state(*twin.engine, *recovered.engine));

    // And the carried-on journal recovers like any other.
    Replica<TypeParam> again;
    const ReplayResult second = journal::replay(file, *again.engine);
    EXPECT_TRUE(second.clean());
    EXPECT_EQ(second.applied, kept + kAfter);
    EXPECT_TRUE(same_state(*twin.engine, *again.engine));
}

TYPED_TEST(Recovery, DamageInTheMiddleStopsTheReplayThereAndIsNotUsable) {
    Recorded<TypeParam> original(test::thin_config(10));
    original.run(200);
    const std::vector<std::size_t> ends = original.ends();
    const std::uint64_t damaged = original.opened + 50;  // records before the bad one

    std::vector<std::byte> file = original.bytes;
    file[ends[damaged] + journal::kRecordHeaderSize + 3] ^= std::byte{0x40};

    Replica<TypeParam> recovered;
    const ReplayResult result = journal::replay(file, *recovered.engine);
    EXPECT_EQ(result.status, ReadStatus::Corrupt);
    EXPECT_FALSE(result.usable());
    EXPECT_FALSE(result.clean());
    EXPECT_EQ(result.applied, damaged);
    EXPECT_EQ(result.good_bytes, ends[damaged]);
    EXPECT_EQ(result.next_sequence, damaged + 1);
    // What was applied is still exactly the records before the damage.
    EXPECT_EQ(digest(*recovered.engine), original.digests[damaged]);
}

// --- Starting part of the way in --------------------------------------------------

// An engine that already holds the first records (because it was restored from
// a snapshot, or because it has replayed an earlier file) is given the rest.
TYPED_TEST(Recovery, ReplayCanStartFromAnyRecord) {
    Recorded<TypeParam> original(test::busy_config(11));
    original.run(600);
    const std::vector<std::size_t> ends = original.ends();
    const std::uint64_t total = original.writer.records();

    for (const std::uint64_t have : {std::uint64_t{0}, original.opened, total / 2, total - 1}) {
        SCOPED_TRACE("the engine already holds " + std::to_string(have) + " records");
        Replica<TypeParam> recovered;
        ASSERT_TRUE(journal::replay(first(original.bytes, ends[have]), *recovered.engine).clean());

        const ReplayResult result = journal::replay(original.bytes, *recovered.engine, have + 1);
        EXPECT_EQ(result.status, ReadStatus::End);
        EXPECT_EQ(result.skipped, have);
        EXPECT_EQ(result.applied, total - have);
        EXPECT_EQ(result.next_sequence, total + 1);
        EXPECT_TRUE(same_state(*original.engine, *recovered.engine));
        EXPECT_EQ(recovered.log.all, original.log.all);
    }
}

TYPED_TEST(Recovery, AnEngineThatAlreadyHoldsEverythingIsLeftAlone) {
    Recorded<TypeParam> original(test::thin_config(12));
    original.run(100);
    const std::uint64_t total = original.writer.records();

    Replica<TypeParam> recovered;
    ASSERT_TRUE(journal::replay(original.bytes, *recovered.engine).clean());
    const std::size_t said = recovered.log.all.size();

    const ReplayResult result = journal::replay(original.bytes, *recovered.engine, total + 1);
    EXPECT_TRUE(result.clean());
    EXPECT_EQ(result.skipped, total);
    EXPECT_EQ(result.applied, 0U);
    EXPECT_EQ(recovered.log.all.size(), said);
    EXPECT_TRUE(same_state(*original.engine, *recovered.engine));
}

// A long-running journal is kept in several files: when one is big enough, a
// new one is started whose header names the next record. Replaying them one
// after another, each from where the last stopped, is replaying the whole.
TYPED_TEST(Recovery, AJournalKeptInSeveralFilesReplaysAsOne) {
    Recorded<TypeParam> original(test::busy_config(13));
    original.run(900);

    // Write the same records again, 250 to a file.
    std::vector<std::vector<std::byte>> files;
    {
        journal::JournalReader reader(original.bytes);
        journal::RecordView record;
        std::unique_ptr<MemoryWriter> writer;
        while (reader.next(record) == ReadStatus::Ok) {
            if ((record.seq - 1) % 250 == 0) {
                if (writer) {
                    writer->flush();
                }
                files.emplace_back();
                writer = std::make_unique<MemoryWriter>(AppendTo{&files.back()}, record.seq);
            }
            switch (record.type) {
                case journal::AddInstrument::kType:
                    writer->append(feed::decode<journal::AddInstrument>(record.body));
                    break;
                case journal::Submit::kType:
                    writer->append(feed::decode<journal::Submit>(record.body));
                    break;
                case journal::Cancel::kType:
                    writer->append(feed::decode<journal::Cancel>(record.body));
                    break;
                default:
                    writer->append(feed::decode<journal::Replace>(record.body));
                    break;
            }
        }
        writer->flush();
    }
    ASSERT_EQ(files.size(), 4U);

    Replica<TypeParam> recovered;
    std::uint64_t next = 1;
    std::uint64_t applied = 0;
    for (const std::vector<std::byte>& file : files) {
        const ReplayResult result = journal::replay(file, *recovered.engine, next);
        ASSERT_TRUE(result.clean());
        EXPECT_EQ(result.skipped, 0U);
        applied += result.applied;
        next = result.next_sequence;
    }
    EXPECT_EQ(applied, original.writer.records());
    EXPECT_TRUE(same_state(*original.engine, *recovered.engine));
    EXPECT_EQ(recovered.log.all, original.log.all);

    // Leaving a file out is noticed, and nothing of the wrong file is applied.
    Replica<TypeParam> gappy;
    const ReplayResult one = journal::replay(files[0], *gappy.engine);
    const std::uint64_t before = digest(*gappy.engine);
    const ReplayResult skipped_a_file = journal::replay(files[2], *gappy.engine, one.next_sequence);
    EXPECT_EQ(skipped_a_file.status, ReadStatus::BadSequence);
    EXPECT_FALSE(skipped_a_file.usable());
    EXPECT_EQ(skipped_a_file.applied, 0U);
    EXPECT_EQ(skipped_a_file.next_sequence, 501U);  // what the file starts with
    EXPECT_EQ(digest(*gappy.engine), before);
}

TYPED_TEST(Recovery, AJournalOlderThanTheEngineIsRefused) {
    Recorded<TypeParam> original(test::thin_config(14));
    original.run(20);
    const std::uint64_t total = original.writer.records();

    // The engine is said to hold records the journal never reached: the
    // journal is from before the snapshot, or it is the wrong file.
    Replica<TypeParam> recovered;
    const ReplayResult result = journal::replay(original.bytes, *recovered.engine, total + 5);
    EXPECT_EQ(result.status, ReadStatus::BadSequence);
    EXPECT_FALSE(result.usable());
    EXPECT_EQ(result.applied, 0U);
    EXPECT_EQ(result.skipped, total);
    EXPECT_TRUE(recovered.log.all.empty());

    // The same with its tail torn off: still the wrong journal.
    Replica<TypeParam> other;
    const ReplayResult torn =
        journal::replay(first(original.bytes, original.bytes.size() - 5), *other.engine, total + 5);
    EXPECT_EQ(torn.status, ReadStatus::BadSequence);
}

TYPED_TEST(Recovery, AnEmptyFileIsAJournalToBeStartedAtTheEnginesNextRecord) {
    Replica<TypeParam> recovered;
    const ReplayResult result = journal::replay({}, *recovered.engine, 42);
    EXPECT_EQ(result.status, ReadStatus::TornTail);
    EXPECT_TRUE(result.usable());
    EXPECT_EQ(result.good_bytes, 0U);
    EXPECT_EQ(result.applied, 0U);
    EXPECT_EQ(result.next_sequence, 42U);
}

TYPED_TEST(Recovery, SomethingThatIsNotAJournalAppliesNothing) {
    std::vector<std::byte> junk(200, std::byte{0x5A});
    Replica<TypeParam> recovered;
    const ReplayResult result = journal::replay(junk, *recovered.engine);
    EXPECT_EQ(result.status, ReadStatus::BadHeader);
    EXPECT_FALSE(result.usable());
    EXPECT_EQ(result.applied, 0U);
    EXPECT_EQ(result.good_bytes, 0U);
    EXPECT_TRUE(recovered.log.all.empty());
}

// --- Writing ahead, and writing in groups ------------------------------------------

// A report sink that notes how big the journal was each time the engine spoke.
struct JournalSizeAtEachReport {
    const std::vector<std::byte>* journal = nullptr;
    std::vector<std::size_t> sizes;

    void note() { sizes.push_back(journal->size()); }
    void on_accepted(const engine::Accepted&) { note(); }
    void on_executed(const engine::Executed&) { note(); }
    void on_cancelled(const engine::Cancelled&) { note(); }
    void on_replaced(const engine::Replaced&) { note(); }
    void on_rejected(const engine::Rejected&) { note(); }
};

// "Write-ahead" means what it says: by the time the engine says anything about
// a request, the request is already in the journal.
TYPED_TEST(Recovery, ARequestIsInTheJournalBeforeTheEngineActsOnIt) {
    using Reports = engine::TeeReports<gen::OrderFlow, JournalSizeAtEachReport>;
    using Engine = typename TypeParam::template Engine<Reports, engine::NullMarketData>;

    std::vector<std::byte> file;
    gen::OrderFlow flow(test::busy_config(15));
    JournalSizeAtEachReport probe{&file, {}};
    Reports reports(flow, probe);
    engine::NullMarketData market;
    auto engine = std::make_unique<Engine>(reports, market);
    MemoryWriter writer(AppendTo{&file});
    journal::Journaled journaled(*engine, writer);
    flow.open(journaled);

    std::uint64_t spoke = 0;
    for (int i = 0; i < 2'000; ++i) {
        probe.sizes.clear();
        const std::size_t before = file.size();
        gen::apply(journaled, flow.next());
        ASSERT_GT(file.size(), before) << "request " << i << " was not recorded";
        ASSERT_EQ(writer.pending(), 0U);
        for (const std::size_t seen : probe.sizes) {
            ASSERT_EQ(seen, file.size()) << "request " << i << ": the engine spoke first";
        }
        spoke += probe.sizes.empty() ? 0U : 1U;
    }
    EXPECT_GT(spoke, 1'900U);  // nearly every request is answered
}

// With group commit the journal trails the engine by up to a batch. A crash
// then loses the requests of the unfinished batch and nothing else: what
// recovery gets is the engine as it was at the last commit.
TYPED_TEST(Recovery, GroupCommitLosesOnlyTheUnfinishedBatch) {
    constexpr std::uint32_t kBatch = 16;
    Recorded<TypeParam> original(test::busy_config(16), kBatch);
    original.run(1'003);
    const std::uint64_t total = original.writer.records();
    const std::uint64_t committed = total / kBatch * kBatch;
    ASSERT_NE(committed, total);  // the run ends inside a batch

    EXPECT_EQ(original.ends().size() - 1, committed);
    EXPECT_GT(original.writer.pending(), 0U);

    Replica<TypeParam> recovered;
    const ReplayResult result = journal::replay(original.bytes, *recovered.engine);
    EXPECT_TRUE(result.clean());
    EXPECT_EQ(result.applied, committed);
    EXPECT_EQ(digest(*recovered.engine), original.digests[committed]);
    EXPECT_NE(digest(*recovered.engine), digest(*original.engine));

    // commit() closes the gap.
    original.journaled.commit();
    EXPECT_EQ(original.writer.pending(), 0U);
    EXPECT_EQ(original.ends().size() - 1, total);
    Replica<TypeParam> whole;
    EXPECT_EQ(journal::replay(original.bytes, *whole.engine).applied, total);
    EXPECT_TRUE(same_state(*original.engine, *whole.engine));
}

TYPED_TEST(Recovery, HowOftenTheJournalIsFlushedDoesNotChangeItsBytes) {
    Recorded<TypeParam> each(test::busy_config(17), 1);
    Recorded<TypeParam> batched(test::busy_config(17), 64);
    Recorded<TypeParam> never(test::busy_config(17), 1'000'000);
    each.run(700);
    batched.run(700);
    never.run(700);
    EXPECT_TRUE(never.bytes.empty());  // not even the header has been handed over
    batched.journaled.commit();
    never.journaled.commit();
    EXPECT_EQ(batched.bytes, each.bytes);
    EXPECT_EQ(never.bytes, each.bytes);
    EXPECT_EQ(batched.log.all, each.log.all);
}

// A batch size of zero would never flush; it is taken to mean one.
TYPED_TEST(Recovery, ABatchOfZeroIsABatchOfOne) {
    Recorded<TypeParam> original(test::thin_config(18), 0);
    original.run(10);
    EXPECT_EQ(original.writer.pending(), 0U);
    EXPECT_EQ(original.ends().size() - 1, original.writer.records());
}

// --- The wrapper is an engine ------------------------------------------------------

TYPED_TEST(Recovery, AJournaledEngineAnswersAsTheEngineItWraps) {
    Recorded<TypeParam> wrapped(test::busy_config(19));
    test::FlowRun<TypeParam> plain(test::busy_config(19));
    wrapped.run(800);
    plain.run(800);

    // The same requests went in, so the same things came out.
    EXPECT_EQ(wrapped.log.all, plain.log.all);
    EXPECT_EQ(wrapped.messages.all, plain.messages.all);

    // And the wrapper passes every query straight through.
    const auto& journaled = wrapped.journaled;
    EXPECT_TRUE(same_state(journaled, *plain.engine));
    EXPECT_EQ(journaled.open_orders(), wrapped.engine->open_orders());
    EXPECT_EQ(&journaled.engine(), wrapped.engine.get());
    for (std::uint32_t i = 0; i < 5; ++i) {
        const auto locate = static_cast<Locate>(i);
        EXPECT_EQ(journaled.listed(locate), plain.engine->listed(locate));
        for (const Side side : {Side::Buy, Side::Sell}) {
            EXPECT_EQ(journaled.best(locate, side), plain.engine->best(locate, side));
        }
    }
}

// Each request the wrapper is given comes back out of the journal as itself,
// and its return value is the engine's.
TYPED_TEST(Recovery, EveryKindOfRequestIsRecordedAsItWasMade) {
    Replica<TypeParam> target;
    std::vector<std::byte> file;
    MemoryWriter writer(AppendTo{&file});
    journal::Journaled journaled(*target.engine, writer);

    const feed::Symbol symbol = feed::Symbol::from("OBEX");
    const engine::NewOrder order{.owner = 3,
                                 .token = 77,
                                 .locate = 5,
                                 .side = Side::Sell,
                                 .qty = 300,
                                 .price = 1'234'500,
                                 .kind = engine::OrderKind::Limit,
                                 .tif = engine::TimeInForce::Day};
    EXPECT_TRUE(journaled.add_instrument(5, symbol, 10));
    EXPECT_FALSE(journaled.add_instrument(5, symbol, 11));  // refused, and still recorded
    const OrderId id = journaled.submit(order, 12);
    ASSERT_NE(id, 0U);
    const OrderId moved = journaled.replace(3, id, 200, 1'234'600, 13);
    EXPECT_NE(moved, 0U);
    EXPECT_NE(moved, id);
    EXPECT_FALSE(journaled.cancel(4, moved, 14));  // somebody else's
    EXPECT_TRUE(journaled.cancel(3, moved, 15));

    journal::JournalReader reader(file);
    journal::RecordView record;
    ASSERT_EQ(reader.next(record), ReadStatus::Ok);
    EXPECT_EQ(feed::decode<journal::AddInstrument>(record.body),
              (journal::AddInstrument{.now = 10, .locate = 5, .symbol = symbol}));
    ASSERT_EQ(reader.next(record), ReadStatus::Ok);
    EXPECT_EQ(feed::decode<journal::AddInstrument>(record.body).now, 11U);
    ASSERT_EQ(reader.next(record), ReadStatus::Ok);
    EXPECT_EQ(feed::decode<journal::Submit>(record.body), journal::Submit::from(order, 12));
    EXPECT_EQ(feed::decode<journal::Submit>(record.body).order(), order);
    ASSERT_EQ(reader.next(record), ReadStatus::Ok);
    EXPECT_EQ(
        feed::decode<journal::Replace>(record.body),
        (journal::Replace{.now = 13, .owner = 3, .order_id = id, .qty = 200, .price = 1'234'600}));
    ASSERT_EQ(reader.next(record), ReadStatus::Ok);
    EXPECT_EQ(feed::decode<journal::Cancel>(record.body),
              (journal::Cancel{.now = 14, .owner = 4, .order_id = moved}));
    ASSERT_EQ(reader.next(record), ReadStatus::Ok);
    EXPECT_EQ(feed::decode<journal::Cancel>(record.body),
              (journal::Cancel{.now = 15, .owner = 3, .order_id = moved}));
    EXPECT_EQ(reader.next(record), ReadStatus::End);
}

// --- The digest --------------------------------------------------------------------

// The recovery tools print the digest and people compare two of them by eye,
// so it has to move for anything that would make two engines behave
// differently. Each case below changes one thing.
template <class Impl>
struct Small {
    Replica<Impl> r;
    Nanos now = 1;

    Small() { r.engine->add_instrument(1, feed::Symbol::from("AAAA"), now++); }

    OrderId rest(Side side, Price price, Qty qty, engine::OwnerId owner = 1,
                 engine::Token token = 1) {
        return r.engine->submit(
            {.owner = owner, .token = token, .locate = 1, .side = side, .qty = qty, .price = price},
            now++);
    }
    [[nodiscard]] std::uint64_t digest() const { return journal::state_digest(*r.engine, 8); }
};

TYPED_TEST(Recovery, TheDigestIsTheSameForTheSameHistory) {
    Small<TypeParam> a;
    Small<TypeParam> b;
    for (Small<TypeParam>* s : {&a, &b}) {
        s->rest(Side::Buy, 10'000, 100);
        s->rest(Side::Sell, 10'100, 200);
    }
    EXPECT_EQ(a.digest(), b.digest());
    EXPECT_NE(a.digest(), Small<TypeParam>().digest());
}

TYPED_TEST(Recovery, TheDigestMovesWithEachThingAnEngineHolds) {
    const auto base = [](Small<TypeParam>& s) {
        s.rest(Side::Buy, 10'000, 100);
        s.rest(Side::Sell, 10'100, 200);
    };
    Small<TypeParam> reference;
    base(reference);
    const std::uint64_t expected = reference.digest();

    {
        SCOPED_TRACE("a different quantity");
        Small<TypeParam> s;
        s.rest(Side::Buy, 10'000, 101);
        s.rest(Side::Sell, 10'100, 200);
        EXPECT_NE(s.digest(), expected);
    }
    {
        SCOPED_TRACE("a different price");
        Small<TypeParam> s;
        s.rest(Side::Buy, 9'900, 100);
        s.rest(Side::Sell, 10'100, 200);
        EXPECT_NE(s.digest(), expected);
    }
    {
        SCOPED_TRACE("a different owner");
        Small<TypeParam> s;
        s.rest(Side::Buy, 10'000, 100, 2);
        s.rest(Side::Sell, 10'100, 200);
        EXPECT_NE(s.digest(), expected);
    }
    {
        SCOPED_TRACE("a different token");
        Small<TypeParam> s;
        s.rest(Side::Buy, 10'000, 100, 1, 2);
        s.rest(Side::Sell, 10'100, 200);
        EXPECT_NE(s.digest(), expected);
    }
    {
        SCOPED_TRACE("the other side");
        Small<TypeParam> s;
        s.rest(Side::Buy, 10'000, 100);
        s.rest(Side::Buy, 9'900, 200);
        EXPECT_NE(s.digest(), expected);
    }
    {
        SCOPED_TRACE("a difference behind the first order of a side");
        Small<TypeParam> a;
        base(a);
        a.rest(Side::Buy, 9'900, 50);
        Small<TypeParam> b;
        base(b);
        b.rest(Side::Buy, 9'900, 51);
        EXPECT_NE(a.digest(), b.digest());
    }
    {
        SCOPED_TRACE("another instrument open");
        Small<TypeParam> s;
        base(s);
        s.r.engine->add_instrument(2, feed::Symbol::from("BBBB"), s.now++);
        EXPECT_NE(s.digest(), expected);
    }
    {
        SCOPED_TRACE("the same book reached another way: the totals differ");
        Small<TypeParam> s;
        base(s);
        const OrderId extra = s.rest(Side::Buy, 9'000, 5);
        ASSERT_TRUE(s.r.engine->cancel(1, extra, s.now++));
        EXPECT_NE(s.digest(), expected);
    }
}

// Two orders at one price in the other order of arrival: the same levels, the
// same shares, a different queue. The digest has to see it, because the next
// trade goes to a different owner.
TYPED_TEST(Recovery, TheDigestSeesWhoIsFirstInAQueue) {
    Small<TypeParam> a;
    a.rest(Side::Buy, 10'000, 100, 1);
    a.rest(Side::Buy, 10'000, 100, 2);
    Small<TypeParam> b;
    b.rest(Side::Buy, 10'000, 100, 2);
    b.rest(Side::Buy, 10'000, 100, 1);
    ASSERT_EQ(test::levels_of(*a.r.engine, 1, Side::Buy),
              test::levels_of(*b.r.engine, 1, Side::Buy));
    EXPECT_NE(a.digest(), b.digest());
}

// The same order on the other side of the book. Nothing about the order
// itself differs (same id, owner, token, price and shares, and the same
// totals), so only the side can tell the two engines apart.
TYPED_TEST(Recovery, TheDigestSeesWhichSideAnOrderIsOn) {
    Small<TypeParam> buying;
    buying.rest(Side::Buy, 10'000, 100);
    Small<TypeParam> selling;
    selling.rest(Side::Sell, 10'000, 100);
    ASSERT_EQ(test::orders_of(*buying.r.engine, 1, Side::Buy),
              test::orders_of(*selling.r.engine, 1, Side::Sell));
    EXPECT_NE(buying.digest(), selling.digest());
}

// What an order carries besides its price and its displayed shares decides
// what it does later: a reserve comes up slice by slice, a display size sets
// how large the slices are and survives a replace, post-only and the
// self-match mode act on the trades to come, and the market reference is what
// the next execution is published under. Each pair of engines below differs
// in exactly one of them.
TYPED_TEST(Recovery, TheDigestSeesWhatAnOrderCarriesBesidesItsPriceAndShares) {
    const auto resting = [](engine::NewOrder order) {
        Small<TypeParam> s;
        order.owner = 1;
        order.token = 1;
        order.locate = 1;
        order.side = Side::Sell;
        order.price = 10'100;
        EXPECT_NE(s.r.engine->submit(order, s.now++), 0U);
        const std::vector<engine::RestingOrder> orders =
            test::orders_of(*s.r.engine, 1, Side::Sell);
        EXPECT_EQ(orders.size(), 1U);
        EXPECT_EQ(orders.empty() ? 0U : orders[0].qty, 100U) << "every case shows 100 shares";
        return s.digest();
    };
    const std::uint64_t plain = resting({.qty = 100});

    // 400 hidden against 300: the same hundred on show.
    EXPECT_NE(resting({.qty = 500, .display = 100}), resting({.qty = 400, .display = 100}))
        << "the reserve";
    // A display size that hides nothing now, and will when the order grows.
    EXPECT_NE(resting({.qty = 100, .display = 100}), plain) << "the display size";
    EXPECT_NE(resting({.qty = 100, .display = 100}), resting({.qty = 100, .display = 150}))
        << "the display size";
    EXPECT_NE(resting({.qty = 100, .post_only = true}), plain) << "post-only";
    EXPECT_NE(resting({.qty = 100, .self_match = engine::SelfMatch::CancelIncoming}), plain)
        << "the self-match mode";
    EXPECT_NE(resting({.qty = 100, .self_match = engine::SelfMatch::CancelResting}),
              resting({.qty = 100, .self_match = engine::SelfMatch::CancelBoth}))
        << "the self-match mode";
}

// An iceberg's new slice is published under the next order number. Two
// engines do the same three things, a trade that brings up a slice and an
// order that comes and goes, in the two possible orders: the same book, the
// same totals, and a slice numbered 4 in one and 3 in the other.
TYPED_TEST(Recovery, TheDigestSeesTheNumberAnIcebergsSliceIsKnownBy) {
    const auto iceberg = [](Small<TypeParam>& s) {
        ASSERT_EQ(s.r.engine->submit({.owner = 1,
                                      .token = 1,
                                      .locate = 1,
                                      .side = Side::Sell,
                                      .qty = 300,
                                      .price = 10'100,
                                      .display = 100},
                                     s.now++),
                  1U);
    };
    const auto trade = [](Small<TypeParam>& s) { s.rest(Side::Buy, 10'100, 100, 2, 7); };
    const auto come_and_go = [](Small<TypeParam>& s) {
        const OrderId id = s.rest(Side::Buy, 9'000, 5, 3, 9);
        ASSERT_TRUE(s.r.engine->cancel(3, id, s.now++));
    };

    Small<TypeParam> a;
    iceberg(a);
    come_and_go(a);  // order 2
    trade(a);        // order 3: the slice becomes 4
    Small<TypeParam> b;
    iceberg(b);
    trade(b);        // order 2: the slice becomes 3
    come_and_go(b);  // order 4

    std::vector<engine::RestingOrder> in_a = test::orders_of(*a.r.engine, 1, Side::Sell);
    std::vector<engine::RestingOrder> in_b = test::orders_of(*b.r.engine, 1, Side::Sell);
    ASSERT_EQ(in_a.size(), 1U);
    ASSERT_EQ(in_b.size(), 1U);
    EXPECT_EQ(in_a[0].market_ref(), 4U);
    EXPECT_EQ(in_b[0].market_ref(), 3U);
    in_a[0].ref = 0;
    in_b[0].ref = 0;
    ASSERT_EQ(in_a, in_b) << "the two orders should differ in their market reference only";
    ASSERT_EQ(a.r.engine->stats(), b.r.engine->stats());
    EXPECT_NE(a.digest(), b.digest());
}

// The same book under another locate is another instrument's book.
TYPED_TEST(Recovery, TheDigestSeesWhichInstrumentABookBelongsTo) {
    const auto with_book_on = [](Locate locate) {
        auto r = std::make_unique<Replica<TypeParam>>();
        r->engine->add_instrument(locate, feed::Symbol::from("AAAA"), 1);
        r->engine->submit({.owner = 1,
                           .token = 1,
                           .locate = locate,
                           .side = Side::Buy,
                           .qty = 100,
                           .price = 10'000,
                           .kind = engine::OrderKind::Limit,
                           .tif = engine::TimeInForce::Day},
                          2);
        return r;
    };
    const auto on_two = with_book_on(2);
    const auto on_five = with_book_on(5);
    ASSERT_EQ(test::orders_of(*on_two->engine, 2, Side::Buy),
              test::orders_of(*on_five->engine, 5, Side::Buy));
    EXPECT_NE(journal::state_digest(*on_two->engine, 8),
              journal::state_digest(*on_five->engine, 8));
}

// It looks only as far as it is told to.
TYPED_TEST(Recovery, TheDigestCoversTheLocatesItIsGiven) {
    Small<TypeParam> s;
    const std::uint64_t before = journal::state_digest(*s.r.engine, 8);
    s.r.engine->add_instrument(20, feed::Symbol::from("FARR"), s.now++);
    EXPECT_EQ(journal::state_digest(*s.r.engine, 8), before);
    EXPECT_NE(journal::state_digest(*s.r.engine, 32), before);
    EXPECT_NE(journal::state_digest(*s.r.engine), before);  // by default, all of them
}

}  // namespace

// --- Things that do not depend on the engine ----------------------------------------

namespace {

TEST(ReplayResult, SaysWhetherTheJournalCanBeCarriedOn) {
    const auto with = [](ReadStatus status) {
        ReplayResult r;
        r.status = status;
        return r;
    };
    EXPECT_TRUE(with(ReadStatus::End).clean());
    EXPECT_TRUE(with(ReadStatus::End).usable());
    EXPECT_FALSE(with(ReadStatus::TornTail).clean());
    EXPECT_TRUE(with(ReadStatus::TornTail).usable());
    for (const ReadStatus bad : {ReadStatus::Corrupt, ReadStatus::BadHeader,
                                 ReadStatus::BadSequence, ReadStatus::UnknownRecord}) {
        EXPECT_FALSE(with(bad).clean());
        EXPECT_FALSE(with(bad).usable());
    }
    EXPECT_TRUE(ReplayResult{}.clean());
}

}  // namespace
