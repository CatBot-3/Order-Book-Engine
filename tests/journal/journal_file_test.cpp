#include <gtest/gtest.h>

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "obe/engine/engines.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/journal/file.hpp"
#include "obe/journal/reader.hpp"
#include "obe/journal/recover.hpp"
#include "obe/journal/replay.hpp"
#include "obe/journal/snapshot.hpp"
#include "obe/journal/writer.hpp"
#include "obe/util/mapped_file.hpp"
#include "support/engine_harness.hpp"
#include "support/flow_run.hpp"
#include "support/journal_run.hpp"

// The journal on a real disk: the file wrapper, the atomic replace a snapshot
// is written with, and recovery from files, including the cut that removes a
// torn tail.

namespace {

using namespace obe;
using journal::JournalFile;
using journal::ReadStatus;
using journal::Recovered;
using journal::SyncPolicy;
using journal::ToFile;

using Impl = engine::ReferenceEngineImpl;
using Replica = test::Replica<Impl>;

// A directory of its own for each test, removed afterwards. The tests run in
// parallel as separate processes, so the name carries the process id.
class JournalFiles : public ::testing::Test {
 protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::path(::testing::TempDir()) /
               ("obe_journal_" + std::to_string(::getpid()) + "_" + info->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(dir_, ignored);
    }

    [[nodiscard]] std::string path(const std::string& name) const { return (dir_ / name).string(); }

    std::filesystem::path dir_;
};

std::vector<std::byte> bytes_of(const std::string& text) {
    std::vector<std::byte> out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::string contents(const std::string& path) {
    const util::MappedFile file(path);
    std::string out;
    for (const std::byte b : file.bytes()) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

std::vector<std::byte> read(const std::string& path) {
    const util::MappedFile file(path);
    return {file.bytes().begin(), file.bytes().end()};
}

// --- JournalFile -------------------------------------------------------------------

TEST_F(JournalFiles, AFileIsCreatedEmptyAndGrowsByWhatIsWritten) {
    const std::string p = path("j");
    JournalFile file(p);
    EXPECT_EQ(file.path(), p);
    EXPECT_EQ(file.size(), 0U);
    file.write(bytes_of("first "));
    file.write(bytes_of("second "));
    file.write(bytes_of("third"));
    EXPECT_EQ(file.size(), 18U);
    EXPECT_EQ(file.writes(), 3U);
    EXPECT_EQ(file.bytes(), 18U);
    EXPECT_EQ(file.syncs(), 0U);
    EXPECT_EQ(contents(p), "first second third");
}

TEST_F(JournalFiles, OpeningAnExistingFileCarriesOnAtItsEnd) {
    const std::string p = path("j");
    {
        JournalFile file(p);
        file.write(bytes_of("before"));
    }
    JournalFile file(p);
    EXPECT_EQ(file.size(), 6U);
    EXPECT_EQ(file.bytes(), 0U);  // counts what this object wrote
    file.write(bytes_of(" after"));
    EXPECT_EQ(contents(p), "before after");
}

TEST_F(JournalFiles, CuttingAFileKeepsItsStartAndTheNextWriteFollowsTheCut) {
    const std::string p = path("j");
    JournalFile file(p);
    file.write(bytes_of("0123456789"));
    file.truncate(4);
    EXPECT_EQ(file.size(), 4U);
    EXPECT_EQ(contents(p), "0123");
    file.write(bytes_of("ab"));
    EXPECT_EQ(contents(p), "0123ab");
}

TEST_F(JournalFiles, SyncingIsCounted) {
    JournalFile file(path("j"));
    file.write(bytes_of("x"));
    file.sync();
    file.sync();
    EXPECT_EQ(file.syncs(), 2U);
}

TEST_F(JournalFiles, AFileThatCannotBeOpenedSaysWhichAndWhy) {
    const std::string p = path("no/such/directory/j");
    try {
        const JournalFile file(p);
        FAIL() << "opened a file in a directory that is not there";
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find(p), std::string::npos) << what;
        EXPECT_NE(what.find("No such file"), std::string::npos) << what;
    }
}

// A journal that cannot take a record must not pretend it did.
TEST_F(JournalFiles, AWriteThatFailsThrowsInsteadOfLosingTheRecord) {
    if (!std::filesystem::exists("/dev/full")) {
        GTEST_SKIP() << "no /dev/full on this system";
    }
    JournalFile file("/dev/full");
    EXPECT_THROW(file.write(bytes_of("a record")), std::runtime_error);
    EXPECT_EQ(file.writes(), 0U);
    EXPECT_EQ(file.bytes(), 0U);
}

TEST_F(JournalFiles, AMovedFileKeepsWorkingAndTheOldObjectLetsGo) {
    const std::string p = path("j");
    auto first = std::make_unique<JournalFile>(p);
    first->write(bytes_of("one"));
    first->sync();
    JournalFile second(std::move(*first));
    first.reset();  // must not close the descriptor the new owner is using
    second.write(bytes_of("two"));
    EXPECT_EQ(second.writes(), 2U);
    EXPECT_EQ(second.syncs(), 1U);
    EXPECT_EQ(second.bytes(), 6U);
    EXPECT_EQ(second.path(), p);
    EXPECT_EQ(contents(p), "onetwo");
}

// --- ToFile ------------------------------------------------------------------------

TEST_F(JournalFiles, EachFlushIsOneWriteAndSyncsOnlyIfAsked) {
    JournalFile lazy(path("lazy"));
    JournalFile careful(path("careful"));
    journal::JournalWriter<ToFile> to_lazy{ToFile{lazy}};  // the default: never sync
    journal::JournalWriter<ToFile> to_careful{ToFile{careful, SyncPolicy::EveryFlush}};
    for (std::uint64_t i = 0; i < 30; ++i) {
        to_lazy.append(journal::Cancel{.now = i, .owner = 1, .order_id = i});
        to_careful.append(journal::Cancel{.now = i, .owner = 1, .order_id = i});
        if (i % 10 == 9) {
            to_lazy.flush();
            to_careful.flush();
        }
    }
    EXPECT_EQ(lazy.writes(), 3U);
    EXPECT_EQ(lazy.syncs(), 0U);
    EXPECT_EQ(careful.writes(), 3U);
    EXPECT_EQ(careful.syncs(), 3U);
    EXPECT_EQ(lazy.bytes(), to_lazy.bytes());
    EXPECT_EQ(read(path("lazy")), read(path("careful")));

    const std::vector<std::byte> written = read(path("lazy"));
    journal::JournalReader reader(written);
    journal::RecordView record;
    std::uint64_t n = 0;
    while (reader.next(record) == ReadStatus::Ok) {
        ++n;
    }
    EXPECT_EQ(n, 30U);
}

// --- write_file_atomically ---------------------------------------------------------

TEST_F(JournalFiles, AFileIsReplacedWholeAndNoTemporaryIsLeft) {
    const std::string p = path("snapshot");
    journal::write_file_atomically(p, bytes_of("the first snapshot, which is the longer one"));
    EXPECT_EQ(contents(p), "the first snapshot, which is the longer one");
    journal::write_file_atomically(p, bytes_of("the second"));
    EXPECT_EQ(contents(p), "the second");
    journal::write_file_atomically(p, {});
    EXPECT_EQ(std::filesystem::file_size(p), 0U);

    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
        ++files;
        EXPECT_EQ(entry.path().filename(), "snapshot");
    }
    EXPECT_EQ(files, 1U);
}

// A process killed while writing a snapshot leaves its temporary file behind.
// The next snapshot reuses the name, and must not inherit anything from it.
TEST_F(JournalFiles, ATemporaryFileLeftByACrashDoesNotLeakIntoTheNextOne) {
    const std::string p = path("snapshot");
    {
        JournalFile stale(p + ".tmp");
        stale.write(bytes_of("what a killed process left: much longer than what follows"));
    }
    journal::write_file_atomically(p, bytes_of("new"));
    EXPECT_EQ(contents(p), "new");
    EXPECT_FALSE(std::filesystem::exists(p + ".tmp"));
}

TEST_F(JournalFiles, AReplaceThatFailsLeavesTheOldFileAsItWas) {
    const std::string p = path("snapshot");
    journal::write_file_atomically(p, bytes_of("the good one"));
    // A directory is in the way of the temporary file, so it cannot be made.
    std::filesystem::create_directory(p + ".tmp");
    EXPECT_THROW(journal::write_file_atomically(p, bytes_of("the new one")), std::runtime_error);
    EXPECT_EQ(contents(p), "the good one");
    // And somewhere that does not exist names the path in the error.
    try {
        journal::write_file_atomically(path("nowhere/snapshot"), bytes_of("x"));
        FAIL();
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("nowhere/snapshot.tmp"), std::string::npos);
    }
}

// --- recover() ---------------------------------------------------------------------

// An engine that journals to a real file while seeded flow trades on it.
struct OnDisk {
    using Reports = engine::TeeReports<gen::OrderFlow, test::ReportLog>;
    using Engine = Impl::Engine<Reports, test::MdLog>;
    using Writer = journal::JournalWriter<ToFile>;

    OnDisk(const std::string& journal_path, const gen::FlowConfig& cfg, std::uint32_t batch = 1)
        : flow(cfg),
          reports(flow, log),
          engine(std::make_unique<Engine>(reports, messages)),
          file(journal_path),
          writer(ToFile{file}),
          journaled(*engine, writer, batch) {
        flow.open(journaled);
    }
    void run(std::uint64_t commands) {
        for (std::uint64_t i = 0; i < commands; ++i) {
            gen::apply(journaled, flow.next());
        }
    }

    gen::OrderFlow flow;
    test::ReportLog log;
    test::MdLog messages;
    Reports reports;
    std::unique_ptr<Engine> engine;
    JournalFile file;
    Writer writer;
    journal::Journaled<Engine, Writer> journaled;
};

constexpr std::uint32_t kLocates = 64;

TEST_F(JournalFiles, AFirstStartFindsNothingAndBeginsAtRecordOne) {
    Replica r;
    const Recovered found = journal::recover(path("j"), path("s"), *r.engine);
    EXPECT_FALSE(found.journal_found);
    EXPECT_FALSE(found.snapshot_found);
    EXPECT_FALSE(found.snapshot_used);
    EXPECT_TRUE(found.ok());
    EXPECT_TRUE(found.file_is_empty());
    EXPECT_EQ(found.next_sequence(), 1U);
    EXPECT_EQ(found.replay.applied, 0U);
    EXPECT_EQ(found.cut, 0U);
    EXPECT_FALSE(std::filesystem::exists(path("j")));  // looking does not create it
}

TEST_F(JournalFiles, AJournalThatEndedCleanlyIsReplayedAndNotTouched) {
    OnDisk original(path("j"), test::busy_config(31));
    original.run(700);
    const std::size_t size = original.file.size();

    Replica r;
    const Recovered found = journal::recover(path("j"), "", *r.engine);
    EXPECT_TRUE(found.journal_found);
    EXPECT_TRUE(found.ok());
    EXPECT_FALSE(found.file_is_empty());
    EXPECT_EQ(found.replay.status, ReadStatus::End);
    EXPECT_EQ(found.replay.applied, original.writer.records());
    EXPECT_EQ(found.next_sequence(), original.writer.next_sequence());
    EXPECT_EQ(found.journal_bytes, size);
    EXPECT_EQ(found.cut, 0U);
    EXPECT_EQ(std::filesystem::file_size(path("j")), size);
    EXPECT_TRUE(test::same_state(*original.engine, *r.engine));
}

// The whole cycle on a real file: a process dies in the middle of a write, the
// next one recovers, cuts the torn tail off, and carries on in the same file.
// The file it ends with is the one a process that never died would have
// written.
TEST_F(JournalFiles, ATornTailIsCutOffAndTheJournalCarriedOnInTheSameFile) {
    const gen::FlowConfig cfg = test::busy_config(32);
    constexpr std::uint64_t kBefore = 400;
    constexpr std::uint64_t kAfter = 300;

    // The one that never crashed.
    {
        OnDisk whole(path("whole"), cfg);
        whole.run(kBefore + kAfter);
    }

    // The one that crashed: kBefore requests, then part of the next record.
    {
        OnDisk crashed(path("j"), cfg);
        crashed.run(kBefore);
        const std::vector<std::byte> reference = read(path("whole"));
        const std::size_t at = crashed.file.size();
        crashed.file.write({reference.data() + at, 11});
    }
    const std::size_t found_size = std::filesystem::file_size(path("j"));

    Replica r;
    const Recovered found = journal::recover(path("j"), "", *r.engine);
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.replay.status, ReadStatus::TornTail);
    EXPECT_EQ(found.journal_bytes, found_size);
    EXPECT_EQ(found.cut, 11U);
    EXPECT_EQ(std::filesystem::file_size(path("j")), found_size - 11);
    EXPECT_EQ(found.replay.applied, cfg.symbols + kBefore);

    // Carry on. The twin supplies the requests that follow.
    test::Recorded<Impl> twin(cfg);
    twin.run(kBefore);
    ASSERT_TRUE(test::same_state(*twin.engine, *r.engine));
    {
        JournalFile file(path("j"));
        journal::JournalWriter<ToFile> writer(ToFile{file}, found.next_sequence(),
                                              found.file_is_empty());
        journal::Journaled journaled(*r.engine, writer);
        for (std::uint64_t i = 0; i < kAfter; ++i) {
            gen::apply(journaled, twin.step());
        }
    }
    EXPECT_EQ(read(path("j")), read(path("whole")));

    // Recovering a second time finds a clean journal and changes nothing.
    Replica again;
    const Recovered second = journal::recover(path("j"), "", *again.engine);
    EXPECT_EQ(second.replay.status, ReadStatus::End);
    EXPECT_EQ(second.cut, 0U);
    EXPECT_TRUE(test::same_state(*twin.engine, *again.engine));
}

TEST_F(JournalFiles, WithoutRepairATornTailIsReportedAndLeftOnDisk) {
    {
        OnDisk original(path("j"), test::thin_config(33));
        original.run(50);
        original.file.write(bytes_of("half a rec"));
    }
    const std::size_t size = std::filesystem::file_size(path("j"));
    Replica r;
    const Recovered found = journal::recover(path("j"), "", *r.engine, /*repair=*/false);
    EXPECT_EQ(found.replay.status, ReadStatus::TornTail);
    EXPECT_EQ(found.replay.good_bytes, size - 10);
    EXPECT_EQ(found.cut, 0U);
    EXPECT_EQ(std::filesystem::file_size(path("j")), size);
}

TEST_F(JournalFiles, AFileThatDiedBeforeItsHeaderIsStartedAgain) {
    {
        std::vector<std::byte> header(journal::kFileHeaderSize);
        journal::encode_file_header(header.data(), 1);
        JournalFile file(path("j"));
        file.write({header.data(), 9});  // the first bytes of a header, and no more
    }
    Replica r;
    const Recovered found = journal::recover(path("j"), "", *r.engine);
    EXPECT_TRUE(found.ok());
    EXPECT_TRUE(found.file_is_empty());
    EXPECT_EQ(found.next_sequence(), 1U);
    EXPECT_EQ(std::filesystem::file_size(path("j")), 0U);

    // What a writer does with those two facts gives a journal that reads.
    {
        JournalFile file(path("j"));
        journal::JournalWriter<ToFile> writer(ToFile{file}, found.next_sequence(),
                                              found.file_is_empty());
        writer.append(journal::Cancel{.now = 1, .owner = 1, .order_id = 1});
        writer.flush();
    }
    Replica again;
    const Recovered second = journal::recover(path("j"), "", *again.engine);
    EXPECT_EQ(second.replay.status, ReadStatus::End);
    EXPECT_EQ(second.replay.applied, 1U);
}

TEST_F(JournalFiles, ADamagedJournalIsReportedAndNeverCut) {
    {
        OnDisk original(path("j"), test::thin_config(34));
        original.run(100);
    }
    std::vector<std::byte> bytes = read(path("j"));
    bytes[bytes.size() / 2] ^= std::byte{0x01};
    journal::write_file_atomically(path("j"), bytes);

    Replica r;
    const Recovered found = journal::recover(path("j"), "", *r.engine);
    EXPECT_FALSE(found.ok());
    EXPECT_EQ(found.replay.status, ReadStatus::Corrupt);
    EXPECT_EQ(found.cut, 0U);
    EXPECT_EQ(read(path("j")), bytes);  // good records follow the damage: nothing is thrown away
}

TEST_F(JournalFiles, ASnapshotSavesReadingTheJournalBeforeIt) {
    OnDisk original(path("j"), test::busy_config(35));
    original.run(600);
    const std::uint64_t at = original.writer.next_sequence();
    journal::write_file_atomically(
        path("s"), journal::encode(journal::capture(*original.engine, at, kLocates)));
    original.run(250);

    Replica r;
    const Recovered found = journal::recover(path("j"), path("s"), *r.engine);
    ASSERT_TRUE(found.ok());
    EXPECT_TRUE(found.snapshot_found);
    EXPECT_TRUE(found.snapshot_used);
    EXPECT_EQ(found.snapshot_sequence, at);
    EXPECT_GT(found.snapshot_orders, 0U);
    EXPECT_EQ(found.replay.skipped, at - 1);
    EXPECT_EQ(found.replay.applied, 250U);
    EXPECT_EQ(found.next_sequence(), original.writer.next_sequence());
    EXPECT_TRUE(test::same_state(*original.engine, *r.engine));
}

// The journal is the record. A snapshot that cannot be trusted is passed over,
// and recovery takes the long way round.
TEST_F(JournalFiles, ADamagedSnapshotIsIgnoredAndTheWholeJournalReplayed) {
    OnDisk original(path("j"), test::busy_config(36));
    original.run(300);
    std::vector<std::byte> snapshot = journal::encode(
        journal::capture(*original.engine, original.writer.next_sequence(), kLocates));
    snapshot[100] ^= std::byte{0x10};
    journal::write_file_atomically(path("s"), snapshot);
    original.run(100);

    Replica r;
    const Recovered found = journal::recover(path("j"), path("s"), *r.engine);
    ASSERT_TRUE(found.ok());
    EXPECT_TRUE(found.snapshot_found);
    EXPECT_FALSE(found.snapshot_used);
    EXPECT_EQ(found.snapshot_sequence, 0U);
    EXPECT_EQ(found.replay.skipped, 0U);
    EXPECT_EQ(found.replay.applied, original.writer.records());
    EXPECT_TRUE(test::same_state(*original.engine, *r.engine));
}

// The pair the rule in recover.hpp is about: a snapshot from after the last
// record the journal still has.
TEST_F(JournalFiles, AJournalThatEndsBeforeItsSnapshotIsNotGuessedAt) {
    OnDisk original(path("j"), test::thin_config(37), /*batch=*/1'000'000);
    original.run(200);
    original.journaled.commit();
    original.run(50);  // these never reach the file
    journal::write_file_atomically(
        path("s"), journal::encode(journal::capture(*original.engine,
                                                    original.writer.next_sequence(), kLocates)));

    Replica r;
    const Recovered found = journal::recover(path("j"), path("s"), *r.engine);
    EXPECT_TRUE(found.snapshot_used);
    EXPECT_FALSE(found.ok());
    EXPECT_EQ(found.replay.status, ReadStatus::BadSequence);
    EXPECT_EQ(found.cut, 0U);
}

// The same snapshot with no journal at all is fine: that is a journal that was
// set aside after the snapshot was taken. A new one starts at the snapshot's
// record.
TEST_F(JournalFiles, ASnapshotAloneStartsANewJournalAtItsRecord) {
    std::uint64_t at = 0;
    {
        OnDisk original(path("old"), test::thin_config(38));
        original.run(120);
        at = original.writer.next_sequence();
        journal::write_file_atomically(
            path("s"), journal::encode(journal::capture(*original.engine, at, kLocates)));

        Replica r;
        const Recovered found = journal::recover(path("j"), path("s"), *r.engine);
        ASSERT_TRUE(found.ok());
        EXPECT_TRUE(found.snapshot_used);
        EXPECT_FALSE(found.journal_found);
        EXPECT_TRUE(found.file_is_empty());
        EXPECT_EQ(found.next_sequence(), at);
        EXPECT_TRUE(test::same_state(*original.engine, *r.engine));
    }
}

}  // namespace
