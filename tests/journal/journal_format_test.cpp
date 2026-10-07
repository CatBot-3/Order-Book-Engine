#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "obe/engine/types.hpp"
#include "obe/feed/codec.hpp"
#include "obe/feed/endian.hpp"
#include "obe/feed/messages.hpp"
#include "obe/journal/reader.hpp"
#include "obe/journal/records.hpp"
#include "obe/journal/writer.hpp"
#include "obe/types.hpp"
#include "obe/util/crc32.hpp"
#include "support/journal_run.hpp"

// The journal's bytes: the checksum, the four records, what the writer lays
// down and what the reader makes of it, whole and damaged.

namespace {

using namespace obe;
using journal::ReadStatus;
using test::AppendTo;
using test::MemoryWriter;

std::vector<std::byte> bytes_of(const std::string& text) {
    std::vector<std::byte> out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

// --- CRC-32 --------------------------------------------------------------------

TEST(Crc32, MatchesThePublishedCheckValues) {
    // The check value every description of CRC-32 quotes.
    EXPECT_EQ(util::crc32(bytes_of("123456789")), 0xCBF43926U);
    EXPECT_EQ(util::crc32(bytes_of("")), 0U);
    EXPECT_EQ(util::crc32(bytes_of("a")), 0xE8B7BE43U);
    EXPECT_EQ(util::crc32(bytes_of("The quick brown fox jumps over the lazy dog")), 0x414FA339U);
    // And a compile-time one: the table is built by the compiler.
    static constexpr std::array<std::byte, 1> kZero{std::byte{0}};
    static_assert(util::crc32(kZero) == 0xD202EF8DU);
}

TEST(Crc32, CanBeComputedInPieces) {
    const std::vector<std::byte> whole = bytes_of("the first half, the second half");
    for (std::size_t cut = 0; cut <= whole.size(); ++cut) {
        const std::span<const std::byte> first(whole.data(), cut);
        const std::span<const std::byte> second(whole.data() + cut, whole.size() - cut);
        ASSERT_EQ(util::crc32(second, util::crc32(first)), util::crc32(whole)) << cut;
    }
}

TEST(Crc32, CatchesEverySingleBitError) {
    std::vector<std::byte> data = bytes_of("a journal record is about this long, give or take");
    const std::uint32_t good = util::crc32(data);
    for (std::size_t i = 0; i < data.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            data[i] ^= static_cast<std::byte>(1 << bit);
            ASSERT_NE(util::crc32(data), good) << "byte " << i << " bit " << bit;
            data[i] ^= static_cast<std::byte>(1 << bit);
        }
    }
    // And a swap of two neighbouring bytes, which a plain sum would miss.
    std::swap(data[3], data[4]);
    EXPECT_NE(util::crc32(data), good);
}

// --- The records ---------------------------------------------------------------

template <class Record>
Record round_trip(const Record& record) {
    std::array<std::byte, journal::kMaxBodySize> buf{};
    EXPECT_EQ(feed::encode(record, buf.data()), feed::wire_size(record));
    EXPECT_EQ(static_cast<char>(buf[0]), Record::kType);
    return feed::decode<Record>(buf.data());
}

TEST(JournalRecords, EachSurvivesEncodingExactly) {
    const journal::AddInstrument open{
        .now = 34'200'000'000'000, .locate = 513, .symbol = feed::Symbol::from("ACME")};
    EXPECT_EQ(round_trip(open), open);

    const journal::Submit submit{.now = 34'200'000'000'123,
                                 .owner = 0xA1B2C3D4,
                                 .token = 0x0102030405060708,
                                 .locate = 7,
                                 .side = Side::Sell,
                                 .qty = 1'234'567,
                                 .price = 4'000'000'000,
                                 .kind = 1,
                                 .tif = 2};
    EXPECT_EQ(round_trip(submit), submit);

    const journal::Cancel cancel{.now = 1, .owner = 2, .order_id = 0xFFFFFFFFFFFFFFFF};
    EXPECT_EQ(round_trip(cancel), cancel);

    const journal::Replace replace{
        .now = 9, .owner = 8, .order_id = 7, .qty = 0xFFFFFFFF, .price = 1};
    EXPECT_EQ(round_trip(replace), replace);
}

TEST(JournalRecords, EachTypeHasItsOwnSizeAndUnknownTypesHaveNone) {
    EXPECT_EQ(journal::body_size('I'), 19U);
    EXPECT_EQ(journal::body_size('S'), 40U);
    EXPECT_EQ(journal::body_size('X'), 21U);
    EXPECT_EQ(journal::body_size('U'), 29U);
    for (int c = 0; c < 256; ++c) {
        const char type = static_cast<char>(c);
        if (type != 'I' && type != 'S' && type != 'X' && type != 'U') {
            ASSERT_EQ(journal::body_size(type), 0U) << c;
        }
    }
}

TEST(JournalRecords, ASubmitKeepsARequestExactlyAsItWasMadeValidOrNot) {
    engine::NewOrder order{.owner = 3,
                           .token = 99,
                           .locate = 4,
                           .side = Side::Buy,
                           .qty = 100,
                           .price = 1'000'000,
                           .kind = engine::OrderKind::Market,
                           .tif = engine::TimeInForce::FillOrKill};
    EXPECT_EQ(round_trip(journal::Submit::from(order, 5)).order(), order);

    // A side that is no side, and enum bytes nobody defined. The engine will
    // reject or ignore them; the journal's job is to hand them back unchanged.
    const auto no_side =
        static_cast<Side>('?');  // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    order.side = no_side;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    order.kind = static_cast<engine::OrderKind>(200);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    order.tif = static_cast<engine::TimeInForce>(7);
    order.qty = 0;
    order.price = 0;
    const journal::Submit kept = round_trip(journal::Submit::from(order, 6));
    EXPECT_EQ(kept.now, 6U);
    EXPECT_EQ(kept.order(), order);
    EXPECT_EQ(static_cast<char>(kept.order().side), '?');
    EXPECT_EQ(static_cast<int>(kept.order().kind), 200);
}

// --- The writer ----------------------------------------------------------------

TEST(JournalWriter, StartsAFileWithItsHeader) {
    std::vector<std::byte> out;
    MemoryWriter writer(AppendTo{&out});
    EXPECT_TRUE(out.empty()) << "nothing is handed over before a flush";
    EXPECT_EQ(writer.pending(), 16U);
    writer.flush();
    // "OBEJ", version 2, zero, first sequence number 1.
    const std::vector<std::byte> expected{
        std::byte{'O'}, std::byte{'B'}, std::byte{'E'}, std::byte{'J'}, std::byte{0}, std::byte{2},
        std::byte{0},   std::byte{0},   std::byte{0},   std::byte{0},   std::byte{0}, std::byte{0},
        std::byte{0},   std::byte{0},   std::byte{0},   std::byte{1}};
    EXPECT_EQ(out, expected);
    EXPECT_EQ(writer.bytes(), 16U);
    EXPECT_EQ(writer.pending(), 0U);
    EXPECT_EQ(writer.next_sequence(), 1U);
}

TEST(JournalWriter, LaysARecordOutAsChecksumSizeSequenceBody) {
    std::vector<std::byte> out;
    MemoryWriter writer(AppendTo{&out});
    writer.flush();
    out.clear();

    const journal::Cancel cancel{.now = 0x0102030405060708, .owner = 0x0A0B0C0D, .order_id = 0x11};
    EXPECT_EQ(writer.append(cancel), 1U);
    writer.flush();
    ASSERT_EQ(out.size(), 14U + 21U);

    // Built by hand from the layout in records.hpp.
    std::vector<std::byte> body;
    for (const int b : {0x00, 0x15,                                // size: 21
                        0,    0,    0,    0,    0, 0, 0, 1,        // sequence 1
                        0x58,                                      // type 'X'
                        1,    2,    3,    4,    5, 6, 7, 8,        // now
                        0x0A, 0x0B, 0x0C, 0x0D,                    // owner
                        0,    0,    0,    0,    0, 0, 0, 0x11}) {  // order id
        body.push_back(static_cast<std::byte>(b));
    }
    EXPECT_EQ(std::vector<std::byte>(out.begin() + 4, out.end()), body);
    EXPECT_EQ(feed::load_be<std::uint32_t>(out.data()), util::crc32(body));
}

TEST(JournalWriter, NumbersRecordsInOrderAndCountsWhatItHandsOver) {
    std::vector<std::byte> out;
    MemoryWriter writer(AppendTo{&out});
    EXPECT_EQ(writer.append(journal::Cancel{}), 1U);
    EXPECT_EQ(writer.append(journal::Replace{}), 2U);
    EXPECT_EQ(writer.append(journal::Submit{}), 3U);
    EXPECT_EQ(writer.records(), 3U);
    EXPECT_EQ(writer.next_sequence(), 4U);
    EXPECT_TRUE(out.empty());
    const std::size_t expected = 16 + (14 + 21) + (14 + 29) + (14 + 40);
    EXPECT_EQ(writer.pending(), expected);
    writer.flush();
    EXPECT_EQ(out.size(), expected);
    EXPECT_EQ(writer.bytes(), expected);
    writer.flush();  // nothing new: nothing handed over
    EXPECT_EQ(out.size(), expected);
}

TEST(JournalWriter, CanCarryOnAnExistingJournalWithoutASecondHeader) {
    std::vector<std::byte> out;
    {
        MemoryWriter first(AppendTo{&out});
        first.append(journal::Cancel{.now = 1});
        first.append(journal::Cancel{.now = 2});
        first.flush();
    }
    MemoryWriter second(AppendTo{&out}, 3, false);
    EXPECT_EQ(second.pending(), 0U);
    EXPECT_EQ(second.append(journal::Cancel{.now = 3}), 3U);
    second.flush();

    journal::JournalReader reader(out);
    journal::RecordView record;
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        ASSERT_EQ(reader.next(record), ReadStatus::Ok);
        EXPECT_EQ(record.seq, seq);
        EXPECT_EQ(feed::decode<journal::Cancel>(record.body).now, seq);
    }
    EXPECT_EQ(reader.next(record), ReadStatus::End);
}

// --- The reader ----------------------------------------------------------------

// A journal of `count` records of mixed types, and where each one ends.
struct Sample {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> ends;  // ends[r]: size with r records
};

Sample sample(std::uint64_t count, std::uint64_t first_seq = 1) {
    Sample s;
    MemoryWriter writer(AppendTo{&s.bytes}, first_seq);
    writer.flush();
    s.ends.push_back(s.bytes.size());
    for (std::uint64_t i = 0; i < count; ++i) {
        switch (i % 4) {
            case 0:
                writer.append(journal::Submit{.now = i, .owner = 1, .token = i, .qty = 100});
                break;
            case 1:
                writer.append(journal::Cancel{.now = i, .owner = 2, .order_id = i});
                break;
            case 2:
                writer.append(journal::Replace{.now = i, .owner = 3, .order_id = i, .qty = 5});
                break;
            default:
                writer.append(journal::AddInstrument{.now = i, .locate = 9});
                break;
        }
        writer.flush();
        s.ends.push_back(s.bytes.size());
    }
    return s;
}

struct ReadAll {
    ReadStatus status = ReadStatus::Ok;
    std::uint64_t records = 0;
    std::size_t offset = 0;
    std::uint64_t next_sequence = 0;
};

ReadAll read_all(std::span<const std::byte> journal) {
    journal::JournalReader reader(journal);
    journal::RecordView record;
    ReadAll out;
    for (;;) {
        out.status = reader.next(record);
        if (out.status != ReadStatus::Ok) {
            break;
        }
        ++out.records;
    }
    out.offset = reader.offset();
    out.next_sequence = reader.next_sequence();
    // Whatever stopped it, asking again gives the same answer.
    EXPECT_EQ(reader.next(record), out.status);
    return out;
}

TEST(JournalReader, ReadsBackWhatWasWrittenInOrder) {
    const Sample s = sample(10);
    journal::JournalReader reader(s.bytes);
    EXPECT_EQ(reader.first_sequence(), 1U);
    journal::RecordView record;
    const std::string types = "SXUISXUISX";
    for (std::uint64_t i = 0; i < 10; ++i) {
        ASSERT_EQ(reader.next(record), ReadStatus::Ok);
        EXPECT_EQ(record.seq, i + 1);
        EXPECT_EQ(record.type, types[i]);
        EXPECT_EQ(record.size, journal::body_size(record.type));
        EXPECT_EQ(reader.offset(), s.ends[i + 1]);
    }
    EXPECT_EQ(reader.next(record), ReadStatus::End);
    EXPECT_EQ(reader.offset(), s.bytes.size());
    EXPECT_EQ(reader.next_sequence(), 11U);
}

TEST(JournalReader, AJournalWithNoRecordsEndsCleanly) {
    const Sample s = sample(0, 500);
    const ReadAll r = read_all(s.bytes);
    EXPECT_EQ(r.status, ReadStatus::End);
    EXPECT_EQ(r.records, 0U);
    EXPECT_EQ(r.offset, 16U);
    EXPECT_EQ(r.next_sequence, 500U) << "the header says where the numbering starts";
}

TEST(JournalReader, AFileThatNeverGotItsHeaderIsACrashAtTheVeryStart) {
    const Sample s = sample(2);
    for (std::size_t length = 0; length < 16; ++length) {
        const ReadAll r = read_all({s.bytes.data(), length});
        EXPECT_EQ(r.status, ReadStatus::TornTail) << length;
        EXPECT_EQ(r.offset, 0U) << "nothing in it is good, not even the header";
        EXPECT_EQ(r.records, 0U);
    }
}

TEST(JournalReader, SomethingThatIsNotAJournalIsRefused) {
    EXPECT_EQ(read_all(bytes_of("PK")).status, ReadStatus::BadHeader);
    EXPECT_EQ(read_all(bytes_of("this is a text file, not a journal")).status,
              ReadStatus::BadHeader);
    // A version this code does not read: the one before (its submit records
    // were shorter) and one that does not exist yet.
    for (const std::uint8_t version : {std::uint8_t{1}, std::uint8_t{3}}) {
        Sample s = sample(3);
        ASSERT_EQ(s.bytes[5], std::byte{2}) << "the current version";
        s.bytes[5] = std::byte{version};
        EXPECT_EQ(read_all(s.bytes).status, ReadStatus::BadHeader);
        EXPECT_EQ(read_all(s.bytes).records, 0U);
    }
}

TEST(JournalReader, AJournalCutOffAnywhereIsATornTailAtTheLastWholeRecord) {
    const Sample s = sample(12);
    std::size_t whole = 0;  // records that fit entirely in the first `length` bytes
    for (std::size_t length = 16; length <= s.bytes.size(); ++length) {
        while (whole + 1 < s.ends.size() && s.ends[whole + 1] <= length) {
            ++whole;
        }
        // A copy of exactly `length` bytes, so that a read beyond them is a
        // read outside an allocation and AddressSanitizer sees it.
        const std::vector<std::byte> cut(s.bytes.begin(),
                                         s.bytes.begin() + static_cast<std::ptrdiff_t>(length));
        const ReadAll r = read_all(cut);
        const bool on_boundary = s.ends[whole] == length;
        ASSERT_EQ(r.status, on_boundary ? ReadStatus::End : ReadStatus::TornTail) << length;
        ASSERT_EQ(r.records, whole) << length;
        ASSERT_EQ(r.offset, s.ends[whole]) << length;
        ASSERT_EQ(r.next_sequence, whole + 1) << length;
    }
}

TEST(JournalReader, ZerosAfterTheLastRecordAreATornTailToo) {
    // What a file looks like when space was set aside for it and the machine
    // stopped before the space was filled.
    Sample s = sample(5);
    const std::size_t good = s.bytes.size();
    s.bytes.resize(good + 4'096, std::byte{0});
    const ReadAll r = read_all(s.bytes);
    EXPECT_EQ(r.status, ReadStatus::TornTail);
    EXPECT_EQ(r.records, 5U);
    EXPECT_EQ(r.offset, good);
}

TEST(JournalReader, DamageInTheLastRecordCannotBeToldFromATornTail) {
    Sample s = sample(6);
    s.bytes[s.ends[5] + 20] ^= std::byte{0x10};
    const ReadAll r = read_all(s.bytes);
    EXPECT_EQ(r.status, ReadStatus::TornTail);
    EXPECT_EQ(r.records, 5U);
    EXPECT_EQ(r.offset, s.ends[5]);
}

TEST(JournalReader, DamageWithGoodRecordsAfterItIsCorruption) {
    const Sample clean = sample(8);
    // One flipped bit in each record but the last, in every field in turn.
    for (std::size_t record = 0; record + 1 < 8; ++record) {
        for (std::size_t at = clean.ends[record]; at < clean.ends[record + 1]; ++at) {
            Sample s = clean;
            s.bytes[at] ^= std::byte{0x04};
            const ReadAll r = read_all(s.bytes);
            ASSERT_EQ(r.status, ReadStatus::Corrupt) << "record " << record << ", byte " << at;
            ASSERT_EQ(r.records, record) << "nothing at or after the damage is handed over";
            ASSERT_EQ(r.offset, clean.ends[record]);
        }
    }
}

// After a crash some filesystems leave old blocks showing at the end of a file
// that was being extended, and those blocks can hold records of an earlier
// journal. An intact record with a number this journal has already passed is
// that: stale, and nothing the journal has not already given. It must not be
// mistaken for a good record beyond damage, or recovery would refuse to cut a
// tail that is safe to cut.
TEST(JournalReader, AnOldRecordAfterATornOneIsStillATornTail) {
    const Sample clean = sample(6);
    const auto record = [&clean](std::size_t n) {  // the bytes of record n (from 1)
        return std::vector<std::byte>(
            clean.bytes.begin() + static_cast<std::ptrdiff_t>(clean.ends[n - 1]),
            clean.bytes.begin() + static_cast<std::ptrdiff_t>(clean.ends[n]));
    };
    // Four whole records, then the first bytes of the fifth.
    std::vector<std::byte> torn(
        clean.bytes.begin(), clean.bytes.begin() + static_cast<std::ptrdiff_t>(clean.ends[4] + 9));

    for (const std::size_t stale : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
        std::vector<std::byte> file = torn;
        const std::vector<std::byte> old = record(stale);
        file.insert(file.end(), old.begin(), old.end());
        const ReadAll r = read_all(file);
        EXPECT_EQ(r.status, ReadStatus::TornTail) << "followed by an old record " << stale;
        EXPECT_EQ(r.records, 4U);
        EXPECT_EQ(r.offset, clean.ends[4]);
    }
    // The fifth itself, or a later one, is another matter: that is a record
    // this journal has not given yet, on the far side of something unreadable.
    for (const std::size_t later : {std::size_t{5}, std::size_t{6}}) {
        std::vector<std::byte> file = torn;
        const std::vector<std::byte> good = record(later);
        file.insert(file.end(), good.begin(), good.end());
        const ReadAll r = read_all(file);
        EXPECT_EQ(r.status, ReadStatus::Corrupt) << "followed by record " << later;
        EXPECT_EQ(r.records, 4U);
        EXPECT_EQ(r.offset, clean.ends[4]);
    }
}

TEST(JournalReader, NoSingleBitErrorAnywhereGetsARecordThrough) {
    // Flip every bit of a journal, one at a time. Whatever the reader calls
    // the result, it must never hand over the damaged record or anything
    // after it.
    const Sample clean = sample(6);
    for (std::size_t at = 0; at < clean.bytes.size(); ++at) {
        std::size_t damaged = 0;  // the record the byte belongs to; the header counts as 0
        while (damaged + 1 < clean.ends.size() && clean.ends[damaged + 1] <= at) {
            ++damaged;
        }
        for (int bit = 0; bit < 8; ++bit) {
            Sample s = clean;
            s.bytes[at] ^= static_cast<std::byte>(1 << bit);
            const ReadAll r = read_all(s.bytes);
            ASSERT_NE(r.status, ReadStatus::End) << "byte " << at << " bit " << bit;
            ASSERT_LE(r.records, at < 16 ? 0U : damaged) << "byte " << at << " bit " << bit;
        }
    }
}

// A record with a valid checksum, built outside the writer.
std::vector<std::byte> raw_record(std::uint64_t seq, const std::vector<std::byte>& body) {
    std::vector<std::byte> out(14 + body.size());
    feed::store_be<std::uint16_t>(out.data() + 4, static_cast<std::uint16_t>(body.size()));
    feed::store_be<std::uint64_t>(out.data() + 6, seq);
    std::copy(body.begin(), body.end(), out.begin() + 14);
    feed::store_be<std::uint32_t>(out.data(), util::crc32({out.data() + 4, out.size() - 4}));
    return out;
}

TEST(JournalReader, AnIntactRecordWithTheWrongNumberIsRefused) {
    // Record 3 of one journal followed by record 5 of another: both whole,
    // both with good checksums, and one request is missing between them.
    Sample s = sample(3);
    const Sample other = sample(5);
    s.bytes.insert(s.bytes.end(), other.bytes.begin() + static_cast<std::ptrdiff_t>(other.ends[4]),
                   other.bytes.end());
    ReadAll r = read_all(s.bytes);
    EXPECT_EQ(r.status, ReadStatus::BadSequence);
    EXPECT_EQ(r.records, 3U);
    EXPECT_EQ(r.offset, s.ends[3]);

    // The same record twice.
    Sample twice = sample(3);
    twice.bytes.insert(twice.bytes.end(),
                       twice.bytes.begin() + static_cast<std::ptrdiff_t>(twice.ends[2]),
                       twice.bytes.begin() + static_cast<std::ptrdiff_t>(twice.ends[3]));
    r = read_all(twice.bytes);
    EXPECT_EQ(r.status, ReadStatus::BadSequence);
    EXPECT_EQ(r.records, 3U);
}

TEST(JournalReader, AnIntactRecordOfAnUnknownTypeOrSizeIsRefused) {
    Sample s = sample(2);
    const std::size_t good = s.bytes.size();

    std::vector<std::byte> unknown = s.bytes;
    const std::vector<std::byte> z = raw_record(3, bytes_of("Zwhatever this is"));
    unknown.insert(unknown.end(), z.begin(), z.end());
    ReadAll r = read_all(unknown);
    EXPECT_EQ(r.status, ReadStatus::UnknownRecord);
    EXPECT_EQ(r.records, 2U);
    EXPECT_EQ(r.offset, good);

    // A known type with the wrong number of bytes: a cancel that is too short.
    std::vector<std::byte> short_cancel = s.bytes;
    const std::vector<std::byte> x = raw_record(3, bytes_of("Xshort"));
    short_cancel.insert(short_cancel.end(), x.begin(), x.end());
    r = read_all(short_cancel);
    EXPECT_EQ(r.status, ReadStatus::UnknownRecord);
    EXPECT_EQ(r.records, 2U);

    // And a record with no body at all.
    std::vector<std::byte> empty = s.bytes;
    const std::vector<std::byte> nothing = raw_record(3, {});
    empty.insert(empty.end(), nothing.begin(), nothing.end());
    EXPECT_EQ(read_all(empty).status, ReadStatus::UnknownRecord);
}

TEST(JournalReader, NamesEveryWayAJournalCanEnd) {
    for (const ReadStatus s :
         {ReadStatus::Ok, ReadStatus::End, ReadStatus::TornTail, ReadStatus::Corrupt,
          ReadStatus::BadHeader, ReadStatus::BadSequence, ReadStatus::UnknownRecord}) {
        EXPECT_FALSE(journal::to_string(s).empty());
        EXPECT_NE(journal::to_string(s), "?");
    }
}

}  // namespace
