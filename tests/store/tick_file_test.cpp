#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/feed/endian.hpp"
#include "obe/feed/messages.hpp"
#include "obe/feed/parser.hpp"
#include "obe/gen/rng.hpp"
#include "obe/store/codecs.hpp"
#include "obe/store/tick_codec.hpp"
#include "obe/store/tick_file.hpp"
#include "obe/types.hpp"
#include "obe/util/crc32.hpp"
#include "support/flow_run.hpp"
#include "support/tick_streams.hpp"

// The tick store (obe/store/tick_file.hpp) around whichever codec the build
// selects: what a writer produces, what a reader gives back for a range of
// time, and what the reader makes of a file that is cut short, damaged, or
// lying.
//
// The files are kept in vectors of exactly their size, so a reader that looks
// one byte past the end of one is looking outside an allocation and
// AddressSanitizer says so.

namespace {

using namespace obe;
using store::BlockInfo;
using store::kBlockHeaderSize;
using store::kEndOfTime;
using store::kFileHeaderSize;
using store::kIndexEntrySize;
using store::kIndexHeaderSize;
using store::kMaxTicksPerBlock;
using store::kTrailerSize;
using store::OpenStatus;
using store::ScanResult;
using store::Tick;
using test::AppendTicksTo;
using test::hostile_ticks;
using test::market_ticks;
using test::scan_all;
using test::store_of;

template <class Codec>
using Reader = store::TickReader<Codec>;
template <class Codec>
using Writer = test::MemoryTickWriter<Codec>;

// The ticks of `all` that a scan of [from, to) must give: the definition,
// with no blocks and no index in it.
std::vector<Tick> between(const std::vector<Tick>& all, Nanos from, Nanos to) {
    std::vector<Tick> out;
    for (const Tick& tick : all) {
        if (tick.timestamp >= from && (to == kEndOfTime || tick.timestamp < to)) {
            out.push_back(tick);
        }
    }
    return out;
}

// Whether `got` is the first got.size() ticks of `all`.
::testing::AssertionResult starts(const std::vector<Tick>& all, const std::vector<Tick>& got) {
    if (got.size() > all.size()) {
        return ::testing::AssertionFailure()
               << got.size() << " ticks came back and only " << all.size() << " were written";
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (!(got[i] == all[i])) {
            return ::testing::AssertionFailure()
                   << "tick " << i << " came back as " << ::testing::PrintToString(got[i])
                   << ", and " << ::testing::PrintToString(all[i]) << " was written";
        }
    }
    return ::testing::AssertionSuccess();
}

std::vector<Tick> first(const std::vector<Tick>& all, std::size_t count) {
    return {all.begin(), all.begin() + static_cast<std::ptrdiff_t>(count)};
}

std::vector<std::byte> first_bytes(const std::vector<std::byte>& all, std::size_t count) {
    return {all.begin(), all.begin() + static_cast<std::ptrdiff_t>(count)};
}

std::size_t end_of(const BlockInfo& block) {
    return static_cast<std::size_t>(block.offset) + kBlockHeaderSize + block.payload;
}

// Where a finished store's trailer says its index begins.
std::size_t index_offset(const std::vector<std::byte>& file) {
    return static_cast<std::size_t>(
        feed::load_be<std::uint64_t>(file.data() + file.size() - kTrailerSize));
}

// Gives an index that was edited the checksum of what it now says.
void reseal_index(std::vector<std::byte>& file) {
    const std::size_t at = index_offset(file);
    const std::size_t end = file.size() - kTrailerSize;
    feed::store_be<std::uint32_t>(file.data() + at + 4,
                                  util::crc32({file.data() + at + 8, end - at - 8}));
}

Tick at_time(Nanos t, std::uint64_t mark) {
    return {7, t, {1'000'000, mark, 1'000'100, 100}};
}

template <class Codec>
class TickFileTest : public ::testing::Test {};
TYPED_TEST_SUITE(TickFileTest, test::CodecTypes);

// --- What a writer produces ----------------------------------------------------------

TYPED_TEST(TickFileTest, AStoreWithNoTicksIsStillAStore) {
    const std::vector<std::byte> file = store_of<TypeParam>({}, 64);
    EXPECT_EQ(file.size(), kFileHeaderSize + kIndexHeaderSize + kTrailerSize);
    const Reader<TypeParam> reader(file);
    EXPECT_EQ(reader.status(), OpenStatus::Complete);
    EXPECT_TRUE(reader.usable());
    EXPECT_EQ(reader.ticks(), 0U);
    EXPECT_TRUE(reader.blocks().empty());
    EXPECT_TRUE(reader.symbols().empty());
    EXPECT_EQ(reader.good_bytes(), file.size());
    const auto scanned = scan_all(reader);
    EXPECT_TRUE(scanned.ticks.empty());
    EXPECT_EQ(scanned.result, ScanResult{});

    // And one whose writer stopped before anything: the header alone.
    const std::vector<std::byte> header = first_bytes(file, kFileHeaderSize);
    const Reader<TypeParam> bare(header);
    EXPECT_EQ(bare.status(), OpenStatus::Recovered);
    EXPECT_TRUE(bare.usable());
    EXPECT_EQ(bare.ticks(), 0U);
    EXPECT_EQ(bare.good_bytes(), kFileHeaderSize);
    EXPECT_EQ(scan_all(bare).result, ScanResult{});
}

TYPED_TEST(TickFileTest, WhatWasWrittenComesBackInTheOrderItWasWritten) {
    const std::vector<std::vector<Tick>> streams = {
        market_ticks(1, 5'000),
        hostile_ticks(2, 5'000),  // times in any order, and the largest time there is
        test::engine_ticks(test::busy_config(3), 10'000),
    };
    for (std::size_t s = 0; s < streams.size(); ++s) {
        const std::vector<Tick>& ticks = streams[s];
        for (const std::uint32_t per_block : {1U, 7U, 256U, store::kDefaultTicksPerBlock}) {
            SCOPED_TRACE(::testing::Message() << "stream " << s << ", " << per_block << " a block");
            const std::vector<std::byte> file = store_of<TypeParam>(ticks, per_block);
            const Reader<TypeParam> reader(file);
            ASSERT_EQ(reader.status(), OpenStatus::Complete);
            EXPECT_EQ(reader.ticks(), ticks.size());
            EXPECT_EQ(reader.ticks_per_block(), per_block);
            EXPECT_EQ(reader.blocks().size(), (ticks.size() + per_block - 1) / per_block);
            EXPECT_EQ(reader.good_bytes(), file.size());

            const auto scanned = scan_all(reader);
            EXPECT_TRUE(scanned.result.ok);
            EXPECT_EQ(scanned.result.ticks, ticks.size());
            EXPECT_EQ(scanned.result.blocks_read, reader.blocks().size());
            EXPECT_EQ(scanned.result.blocks_skipped, 0U);
            EXPECT_EQ(scanned.result.bad_block, ScanResult::kNone);
            ASSERT_EQ(scanned.ticks.size(), ticks.size());
            EXPECT_TRUE(starts(ticks, scanned.ticks));
        }
    }
}

TYPED_TEST(TickFileTest, TheFileBeginsWithItsHeaderAndEndsWithItsTrailer) {
    const std::vector<std::byte> file = store_of<TypeParam>(market_ticks(4, 10), 300);
    EXPECT_EQ(std::memcmp(file.data(), "OBET", 4), 0);
    EXPECT_EQ(feed::load_be<std::uint16_t>(file.data() + 4), 1U);
    EXPECT_EQ(feed::load_be<std::uint16_t>(file.data() + 6), TypeParam::kId);
    EXPECT_EQ(feed::load_be<std::uint32_t>(file.data() + 8), 300U);
    EXPECT_EQ(feed::load_be<std::uint32_t>(file.data() + 12), 0U);
    EXPECT_EQ(std::memcmp(file.data() + kFileHeaderSize, "BLK1", 4), 0);
    EXPECT_EQ(std::memcmp(file.data() + file.size() - 4, "OBEX", 4), 0);
    EXPECT_EQ(std::memcmp(file.data() + index_offset(file), "IDX1", 4), 0);

    const std::optional<store::FileHeader> header = store::read_header(file);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->version, store::kFileVersion);
    EXPECT_EQ(header->codec, TypeParam::kId);
    EXPECT_EQ(header->ticks_per_block, 300U);
}

TYPED_TEST(TickFileTest, BlocksAreCutEverySoManyTicksAndTheIndexDescribesEachOne) {
    // Times in no order, so that a block's earliest and latest are not simply
    // its first and last tick.
    const std::vector<Tick> ticks = hostile_ticks(5, 1'000);
    std::vector<std::byte> file;
    Writer<TypeParam> writer(AppendTicksTo{&file}, 300);
    EXPECT_EQ(file.size(), kFileHeaderSize) << "the header is written at once";
    EXPECT_EQ(writer.bytes(), kFileHeaderSize);
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        writer.append(ticks[i]);
        ASSERT_EQ(writer.blocks(), (i + 1) / 300) << "a block is handed over when it is full";
        ASSERT_EQ(writer.bytes(), file.size());
    }
    EXPECT_EQ(writer.ticks(), 1'000U);
    EXPECT_FALSE(writer.finished());

    writer.finish();
    EXPECT_TRUE(writer.finished());
    EXPECT_EQ(writer.blocks(), 4U);
    EXPECT_EQ(writer.bytes(), file.size());
    const std::vector<std::byte> finished = file;
    writer.finish();
    EXPECT_EQ(file, finished) << "finishing twice wrote something";

    const Reader<TypeParam> reader(file);
    ASSERT_EQ(reader.status(), OpenStatus::Complete);
    const std::vector<BlockInfo>& blocks = reader.blocks();
    ASSERT_EQ(blocks.size(), 4U);
    std::size_t at = kFileHeaderSize;
    std::uint64_t payload = 0;
    for (std::size_t k = 0; k < blocks.size(); ++k) {
        const std::size_t count = k < 3 ? 300 : 100;
        Nanos earliest = ticks[k * 300].timestamp;
        Nanos latest = earliest;
        for (std::size_t i = k * 300; i < k * 300 + count; ++i) {
            earliest = std::min(earliest, ticks[i].timestamp);
            latest = std::max(latest, ticks[i].timestamp);
        }
        EXPECT_EQ(blocks[k].offset, at) << "block " << k;
        EXPECT_EQ(blocks[k].count, count) << "block " << k;
        EXPECT_EQ(blocks[k].earliest, earliest) << "block " << k;
        EXPECT_EQ(blocks[k].latest, latest) << "block " << k;
        EXPECT_GE(blocks[k].payload, count);
        EXPECT_LE(blocks[k].payload, count * TypeParam::kMaxTickSize);
        at = end_of(blocks[k]);
        payload += blocks[k].payload;
    }
    EXPECT_EQ(index_offset(file), at) << "the index begins where the last block ends";
    EXPECT_EQ(writer.payload_bytes(), payload);
    EXPECT_EQ(file.size(), at + kIndexHeaderSize + blocks.size() * kIndexEntrySize + kTrailerSize);
}

TYPED_TEST(TickFileTest, ALastBlockThatIsExactlyFullLeavesNoEmptyOneBehind) {
    const std::vector<Tick> ticks = market_ticks(6, 600);
    const std::vector<std::byte> file = store_of<TypeParam>(ticks, 300);
    const Reader<TypeParam> reader(file);
    ASSERT_EQ(reader.status(), OpenStatus::Complete);
    ASSERT_EQ(reader.blocks().size(), 2U);
    EXPECT_EQ(reader.blocks()[0].count, 300U);
    EXPECT_EQ(reader.blocks()[1].count, 300U);
    EXPECT_EQ(scan_all(reader).ticks, ticks);
}

TYPED_TEST(TickFileTest, TheBlockSizeIsHeldBetweenOneAndTheLimit) {
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> cases = {
        {0, 1},
        {1, 1},
        {kMaxTicksPerBlock, kMaxTicksPerBlock},
        {kMaxTicksPerBlock + 1, kMaxTicksPerBlock},
        {0xFFFF'FFFFU, kMaxTicksPerBlock},
    };
    const std::vector<Tick> ticks = market_ticks(7, 5);
    for (const auto& [asked, given] : cases) {
        std::vector<std::byte> file;
        Writer<TypeParam> writer(AppendTicksTo{&file}, asked);
        EXPECT_EQ(writer.ticks_per_block(), given) << "asked for " << asked;
        for (const Tick& tick : ticks) {
            writer.append(tick);
        }
        writer.finish();
        const Reader<TypeParam> reader(file);
        ASSERT_EQ(reader.status(), OpenStatus::Complete) << "asked for " << asked;
        EXPECT_EQ(reader.ticks_per_block(), given);
        EXPECT_EQ(scan_all(reader).ticks, ticks);
    }
    std::vector<std::byte> file;
    const Writer<TypeParam> by_default(AppendTicksTo{&file});
    EXPECT_EQ(by_default.ticks_per_block(), 4096U);
}

// --- Queries by time -----------------------------------------------------------------

TYPED_TEST(TickFileTest, ATimeRangeGivesExactlyTheTicksInsideIt) {
    const std::vector<std::vector<Tick>> streams = {
        market_ticks(11, 6'000),   // time rising, as on a feed
        hostile_ticks(12, 3'000),  // time anywhere: every block overlaps every other
    };
    for (std::size_t s = 0; s < streams.size(); ++s) {
        const std::vector<Tick>& ticks = streams[s];
        const std::vector<std::byte> file = store_of<TypeParam>(ticks, 128);
        const Reader<TypeParam> reader(file);
        ASSERT_EQ(reader.status(), OpenStatus::Complete);

        gen::SplitMix64 rng(13 + s);
        const auto some_time = [&]() -> Nanos {
            // Mostly at or beside a time that is in the store, where an
            // off-by-one would show.
            const Nanos t = ticks[rng.below(ticks.size())].timestamp;
            switch (rng.below(6)) {
                case 0:
                    return t == 0 ? 0 : t - 1;
                case 1:
                    return t == kEndOfTime ? t : t + 1;
                case 2:
                    return rng.next();
                default:
                    return t;
            }
        };
        std::uint64_t given = 0;
        for (int round = 0; round < 400; ++round) {
            Nanos from = some_time();
            Nanos to = some_time();
            if (round % 50 == 0) {
                from = 0;
            }
            if (round % 50 == 25) {
                to = kEndOfTime;
            }
            SCOPED_TRACE(::testing::Message()
                         << "stream " << s << ", from " << from << " to " << to);
            const std::vector<Tick> expected = between(ticks, from, to);
            const auto scanned = scan_all(reader, from, to);
            ASSERT_TRUE(scanned.result.ok);
            ASSERT_EQ(scanned.ticks.size(), expected.size());
            ASSERT_TRUE(starts(expected, scanned.ticks));
            EXPECT_EQ(scanned.result.ticks, expected.size());
            given += expected.size();

            // Exactly the blocks whose own range touches the query are read.
            std::uint64_t touching = 0;
            for (const BlockInfo& block : reader.blocks()) {
                if (block.latest >= from && (to == kEndOfTime || block.earliest < to)) {
                    ++touching;
                }
            }
            EXPECT_EQ(scanned.result.blocks_read, touching);
            EXPECT_EQ(scanned.result.blocks_skipped, reader.blocks().size() - touching);
        }
        EXPECT_GT(given, 10'000U) << "the ranges were nearly all empty: the test tested little";
    }
}

TYPED_TEST(TickFileTest, ARangeTakesItsStartAndLeavesItsEnd) {
    const std::vector<Tick> ticks = {
        at_time(10, 1), at_time(20, 2), at_time(20, 3), at_time(30, 4), at_time(kEndOfTime, 5),
    };
    const std::vector<std::byte> file = store_of<TypeParam>(ticks, 2);
    const Reader<TypeParam> reader(file);
    ASSERT_EQ(reader.status(), OpenStatus::Complete);
    const auto marks = [&reader](Nanos from, Nanos to) {
        std::vector<std::uint64_t> out;
        for (const Tick& tick : scan_all(reader, from, to).ticks) {
            out.push_back(tick.bbo.bid_qty);
        }
        return out;
    };
    using Marks = std::vector<std::uint64_t>;
    EXPECT_EQ(marks(20, 30), (Marks{2, 3}));
    EXPECT_EQ(marks(20, 21), (Marks{2, 3}));
    EXPECT_EQ(marks(20, 20), Marks{}) << "an empty range";
    EXPECT_EQ(marks(21, 30), Marks{});
    EXPECT_EQ(marks(10, 11), Marks{1});
    EXPECT_EQ(marks(0, 10), Marks{});
    EXPECT_EQ(marks(11, 31), (Marks{2, 3, 4}));
    EXPECT_EQ(marks(30, 20), Marks{}) << "a range that ends before it begins";
    // A range cannot name an end after the largest time, so kEndOfTime as the
    // end means there is none.
    EXPECT_EQ(marks(0, kEndOfTime - 1), (Marks{1, 2, 3, 4}));
    EXPECT_EQ(marks(30, kEndOfTime), (Marks{4, 5}));
    EXPECT_EQ(marks(kEndOfTime, kEndOfTime), Marks{5});
    EXPECT_EQ(marks(0, kEndOfTime), (Marks{1, 2, 3, 4, 5}));

    std::vector<std::uint64_t> all;
    const ScanResult result = reader.scan([&all](const Tick& tick) {
        all.push_back(tick.bbo.bid_qty);
        return true;
    });
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(all, (Marks{1, 2, 3, 4, 5})) << "a scan of everything left something out";
}

// What the index is for: on a store in time order, a short range costs a
// block or two however long the day was.
TYPED_TEST(TickFileTest, TheIndexSparesAQueryTheBlocksItCannotNeed) {
    const std::vector<Tick> ticks = market_ticks(14, 6'400);
    const std::vector<std::byte> file = store_of<TypeParam>(ticks, 100);
    const Reader<TypeParam> reader(file);
    ASSERT_EQ(reader.blocks().size(), 64U);

    const BlockInfo& block = reader.blocks()[20];
    const auto inside = scan_all(reader, block.earliest, block.latest + 1);
    EXPECT_TRUE(inside.result.ok);
    EXPECT_GE(inside.ticks.size(), 100U);
    EXPECT_GE(inside.result.blocks_read, 1U);
    EXPECT_LE(inside.result.blocks_read, 3U) << "the block and at most its two neighbours";
    EXPECT_EQ(inside.result.blocks_read + inside.result.blocks_skipped, 64U);
    EXPECT_EQ(inside.ticks, between(ticks, block.earliest, block.latest + 1));

    const auto before = scan_all(reader, 0, ticks.front().timestamp);
    EXPECT_TRUE(before.ticks.empty());
    EXPECT_EQ(before.result.blocks_read, 0U);
    EXPECT_EQ(before.result.blocks_skipped, 64U);

    const auto after = scan_all(reader, ticks.back().timestamp + 1, kEndOfTime);
    EXPECT_TRUE(after.ticks.empty());
    EXPECT_EQ(after.result.blocks_read, 0U);
    EXPECT_EQ(after.result.blocks_skipped, 64U);
}

TYPED_TEST(TickFileTest, AVisitorCanStopAScanAndThatIsNotAnError) {
    const std::vector<Tick> ticks = market_ticks(15, 1'000);
    const std::vector<std::byte> file = store_of<TypeParam>(ticks, 100);
    const Reader<TypeParam> reader(file);
    for (const std::size_t wanted : {std::size_t{1}, std::size_t{99}, std::size_t{100},
                                     std::size_t{101}, std::size_t{250}, std::size_t{1'000}}) {
        std::vector<Tick> seen;
        const ScanResult result = reader.scan([&](const Tick& tick) {
            seen.push_back(tick);
            return seen.size() < wanted;
        });
        EXPECT_TRUE(result.ok) << wanted;
        EXPECT_EQ(result.bad_block, ScanResult::kNone);
        EXPECT_EQ(seen, first(ticks, wanted)) << "the visitor was called after it said stop";
        EXPECT_EQ(result.ticks, wanted);
        EXPECT_EQ(result.blocks_read, (wanted + 99) / 100) << "blocks beyond the stop were read";
    }
}

// --- The symbol directory ------------------------------------------------------------

TYPED_TEST(TickFileTest, TheDirectoryComesBackInLocateOrderAndTheLastNameForALocateStands) {
    std::vector<std::byte> file;
    Writer<TypeParam> writer(AppendTicksTo{&file}, 16);
    writer.add_symbol(500, feed::Symbol::from("MSFT"));
    writer.add_symbol(2, feed::Symbol::from("AAPL"));
    writer.add_symbol(65'535, feed::Symbol::from("ZZZZZZZZ"));
    writer.add_symbol(500, feed::Symbol::from("META"));  // the locate was named again
    for (const Tick& tick : market_ticks(16, 40)) {
        writer.append(tick);
    }
    writer.finish();

    const Reader<TypeParam> reader(file);
    ASSERT_EQ(reader.status(), OpenStatus::Complete);
    const std::vector<store::SymbolEntry> expected = {
        {2, feed::Symbol::from("AAPL")},
        {500, feed::Symbol::from("META")},
        {65'535, feed::Symbol::from("ZZZZZZZZ")},
    };
    EXPECT_EQ(reader.symbols(), expected);
    EXPECT_EQ(reader.locate_of("AAPL"), std::optional<Locate>(2));
    EXPECT_EQ(reader.locate_of("META"), std::optional<Locate>(500));
    EXPECT_EQ(reader.locate_of("ZZZZZZZZ"), std::optional<Locate>(65'535));
    EXPECT_EQ(reader.locate_of("MSFT"), std::nullopt) << "the name that was replaced";
    EXPECT_EQ(reader.locate_of("AAP"), std::nullopt);
    EXPECT_EQ(reader.locate_of(""), std::nullopt);
    EXPECT_EQ(reader.ticks(), 40U) << "the directory disturbed the ticks";
}

// --- A recorder that stopped ---------------------------------------------------------

TYPED_TEST(TickFileTest, AStoreThatWasNeverFinishedIsReadUpToItsLastWholeBlock) {
    const std::vector<Tick> ticks = market_ticks(21, 1'000);
    std::vector<std::byte> file;
    {
        Writer<TypeParam> writer(AppendTicksTo{&file}, 300);
        writer.add_symbol(1, feed::Symbol::from("AAPL"));
        for (const Tick& tick : ticks) {
            writer.append(tick);
        }
        // Killed here: the hundred ticks of the fourth block were still in
        // memory, and the index was never written.
    }
    const Reader<TypeParam> reader(file);
    EXPECT_EQ(reader.status(), OpenStatus::Recovered);
    EXPECT_TRUE(reader.usable());
    EXPECT_EQ(reader.ticks(), 900U);
    ASSERT_EQ(reader.blocks().size(), 3U);
    EXPECT_EQ(reader.good_bytes(), file.size());
    EXPECT_TRUE(reader.symbols().empty()) << "the names live in the index, which is not there";
    const auto scanned = scan_all(reader);
    EXPECT_TRUE(scanned.result.ok);
    EXPECT_EQ(scanned.ticks, first(ticks, 900));

    // The blocks carry their own time ranges, so queries work as before.
    const Nanos from = ticks[350].timestamp;
    const Nanos to = ticks[420].timestamp;
    const auto ranged = scan_all(reader, from, to);
    EXPECT_EQ(ranged.ticks, between(first(ticks, 900), from, to));
    EXPECT_GE(ranged.result.blocks_skipped, 1U);

    // With the start of a block after them that never got its ticks.
    std::vector<std::byte> torn = file;
    const std::size_t one_block = end_of(reader.blocks()[0]) - kFileHeaderSize;
    torn.insert(torn.end(), file.begin() + kFileHeaderSize,
                file.begin() + static_cast<std::ptrdiff_t>(kFileHeaderSize + one_block - 1));
    const Reader<TypeParam> after_torn(torn);
    EXPECT_EQ(after_torn.status(), OpenStatus::Recovered);
    EXPECT_EQ(after_torn.ticks(), 900U);
    EXPECT_EQ(after_torn.good_bytes(), file.size()) << "where the whole blocks end";
    EXPECT_EQ(scan_all(after_torn).ticks, first(ticks, 900));
}

// The whole of the claim above, for every place a file can end.
TYPED_TEST(TickFileTest, CutAnywhereAStoreGivesBackItsWholeBlocksAndNothingElse) {
    const std::vector<Tick> ticks = market_ticks(22, 60, 5);
    std::vector<std::byte> full;
    {
        Writer<TypeParam> writer(AppendTicksTo{&full}, 8);
        writer.add_symbol(1, feed::Symbol::from("AAPL"));
        for (const Tick& tick : ticks) {
            writer.append(tick);
        }
        writer.finish();
    }
    const Reader<TypeParam> whole(full);
    ASSERT_EQ(whole.status(), OpenStatus::Complete);
    const std::vector<BlockInfo>& blocks = whole.blocks();
    ASSERT_EQ(blocks.size(), 8U);

    for (std::size_t cut = 0; cut < full.size(); ++cut) {
        SCOPED_TRACE(::testing::Message() << "cut to " << cut << " of " << full.size() << " bytes");
        const std::vector<std::byte> part = first_bytes(full, cut);
        const Reader<TypeParam> reader(part);
        if (cut < kFileHeaderSize) {
            ASSERT_EQ(reader.status(), OpenStatus::BadHeader);
            ASSERT_FALSE(reader.usable());
            ASSERT_EQ(scan_all(reader).result, ScanResult{});
            continue;
        }
        ASSERT_EQ(reader.status(), OpenStatus::Recovered) << "only the whole file has its index";
        std::size_t whole_blocks = 0;
        std::size_t whole_ticks = 0;
        std::size_t good = kFileHeaderSize;
        for (const BlockInfo& block : blocks) {
            if (end_of(block) <= cut) {
                ++whole_blocks;
                whole_ticks += block.count;
                good = end_of(block);
            }
        }
        ASSERT_EQ(reader.blocks().size(), whole_blocks);
        ASSERT_EQ(reader.ticks(), whole_ticks);
        ASSERT_EQ(reader.good_bytes(), good);
        ASSERT_TRUE(reader.symbols().empty());
        const auto scanned = scan_all(reader);
        ASSERT_TRUE(scanned.result.ok);
        ASSERT_EQ(scanned.ticks, first(ticks, whole_ticks));
    }
}

// --- Damage --------------------------------------------------------------------------

// The promise that matters most: whatever happens to the bytes, a reader
// never hands over a tick that was not written, and when ticks are missing it
// says so, either in how the store opened or in how the scan ended.
TYPED_TEST(TickFileTest, AFlippedBitNeverPutsATickThereThatWasNotWritten) {
    const std::vector<Tick> ticks = market_ticks(31, 60, 5);
    std::vector<std::byte> file;
    {
        Writer<TypeParam> writer(AppendTicksTo{&file}, 8);
        writer.add_symbol(1, feed::Symbol::from("AAPL"));
        for (const Tick& tick : ticks) {
            writer.append(tick);
        }
        writer.finish();
    }
    const Reader<TypeParam> intact(file);
    const std::vector<BlockInfo>& blocks = intact.blocks();
    const std::vector<store::SymbolEntry>& names = intact.symbols();
    ASSERT_EQ(names.size(), 1U);
    int refused = 0;
    int recovered = 0;
    int found_when_read = 0;
    int harmless = 0;
    for (std::size_t at = 0; at < file.size(); ++at) {
        // Every bit of the headers at both ends, where each field has its own
        // check, and one bit of every byte between.
        const bool every_bit = at < 64 || at + 128 >= file.size();
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (!every_bit && bit != at % 8) {
                continue;
            }
            SCOPED_TRACE(::testing::Message() << "bit " << bit << " of byte " << at);
            std::vector<std::byte> damaged = file;
            damaged[at] ^= static_cast<std::byte>(1U << bit);
            const Reader<TypeParam> reader(damaged);
            const auto scanned = scan_all(reader);
            ASSERT_TRUE(starts(ticks, scanned.ticks));
            // What it says about its blocks is true of them: where each is,
            // how many ticks it holds, and the range of time a query would
            // rule it in or out by.
            ASSERT_LE(reader.blocks().size(), blocks.size());
            for (std::size_t k = 0; k < reader.blocks().size(); ++k) {
                ASSERT_EQ(reader.blocks()[k], blocks[k]) << "block " << k;
            }
            if (!reader.usable()) {
                ASSERT_TRUE(scanned.ticks.empty());
                ++refused;
            } else if (reader.status() == OpenStatus::Recovered) {
                ASSERT_TRUE(scanned.result.ok) << "a recovered store holds only blocks it checked";
                ASSERT_EQ(scanned.ticks.size(), reader.ticks());
                ++recovered;
            } else if (!scanned.result.ok) {
                ASSERT_NE(scanned.result.bad_block, ScanResult::kNone);
                ASSERT_EQ(scanned.ticks.size(), 8 * scanned.result.bad_block)
                    << "ticks of the damaged block, or after it, were handed over";
                ++found_when_read;
            } else {
                ASSERT_EQ(scanned.ticks.size(), ticks.size())
                    << "ticks are missing and nothing said so";
                ASSERT_EQ(reader.symbols(), names) << "a damaged directory was believed";
                ++harmless;
            }
        }
    }
    // Every kind of outcome happened, or the test is not testing what it says.
    EXPECT_GT(refused, 0) << "damage to the file header";
    EXPECT_GT(recovered, 0) << "damage to the index or to a block header";
    EXPECT_GT(found_when_read, 0) << "damage to the ticks themselves";
    EXPECT_GT(harmless, 0) << "the block size in the header may grow without harm";
}

TYPED_TEST(TickFileTest, ABlockDamagedAfterItWasWrittenStopsOnlyTheScansThatReachIt) {
    const std::vector<Tick> ticks = market_ticks(32, 1'000);
    const std::vector<std::byte> file = store_of<TypeParam>(ticks, 100);
    const std::vector<BlockInfo> blocks = Reader<TypeParam>(file).blocks();
    ASSERT_EQ(blocks.size(), 10U);
    ASSERT_LT(blocks[5].latest, blocks[7].earliest);

    // Once in the middle of the ticks, once in the checksum itself.
    for (const std::size_t where :
         {static_cast<std::size_t>(blocks[5].offset) + kBlockHeaderSize + blocks[5].payload / 2,
          static_cast<std::size_t>(blocks[5].offset) + 5}) {
        std::vector<std::byte> damaged = file;
        damaged[where] ^= std::byte{0x10};
        const Reader<TypeParam> reader(damaged);
        // The index and the block headers still agree: the damage is found
        // when the block is read, not when the store is opened.
        ASSERT_EQ(reader.status(), OpenStatus::Complete);
        EXPECT_EQ(reader.ticks(), 1'000U);

        const auto all = scan_all(reader);
        EXPECT_FALSE(all.result.ok);
        EXPECT_EQ(all.result.bad_block, 5U);
        EXPECT_EQ(all.result.blocks_read, 5U);
        EXPECT_EQ(all.ticks, first(ticks, 500));

        const auto earlier = scan_all(reader, 0, blocks[4].earliest);
        EXPECT_TRUE(earlier.result.ok);
        EXPECT_EQ(earlier.ticks, between(ticks, 0, blocks[4].earliest));

        // The blocks after it do not depend on it: each starts from a reset.
        const auto later = scan_all(reader, blocks[7].earliest, kEndOfTime);
        EXPECT_TRUE(later.result.ok);
        EXPECT_EQ(later.result.bad_block, ScanResult::kNone);
        EXPECT_EQ(later.ticks, between(ticks, blocks[7].earliest, kEndOfTime));
        EXPECT_FALSE(later.ticks.empty());
    }
}

// The checksum of an index says its bytes are the ones that were written. It
// does not say they are true. A reader that believed an index whose ranges
// were wrong would skip blocks that hold the answer, and say nothing.
TYPED_TEST(TickFileTest, AnIndexThatChecksOutButDoesNotMatchTheBlocksIsNotBelieved) {
    const std::vector<Tick> ticks = market_ticks(33, 240);
    const std::vector<std::byte> file = store_of<TypeParam>(ticks, 80);
    const std::size_t index = index_offset(file);
    {
        std::vector<std::byte> same = file;
        reseal_index(same);
        ASSERT_EQ(same, file) << "the test's own checksum of the index is not the writer's";
    }
    const std::size_t entry = index + kIndexHeaderSize + 1 * kIndexEntrySize;  // of block 1
    struct Lie {
        std::string_view what;
        std::size_t at;     // of a big-endian field
        std::size_t width;  // 4 or 8
        std::int64_t by;
    };
    const std::vector<Lie> lies = {
        {"a block starts a byte later", entry, 8, 1},
        {"a block holds one tick more", entry + 8, 4, 1},
        {"a block holds one tick fewer", entry + 8, 4, -1},
        {"a block is a byte shorter", entry + 12, 4, -1},
        {"a block begins later in time", entry + 16, 8, 1},
        {"a block ends earlier in time", entry + 24, 8, -1},
        {"the store holds one tick more", index + 16, 8, 1},
        {"there is one block more", index + 8, 4, 1},
        {"there is one name more", index + 12, 4, 1},
    };
    for (const Lie& lie : lies) {
        SCOPED_TRACE(lie.what);
        std::vector<std::byte> lying = file;
        std::byte* p = lying.data() + lie.at;
        if (lie.width == 4) {
            feed::store_be<std::uint32_t>(
                p, static_cast<std::uint32_t>(feed::load_be<std::uint32_t>(p) +
                                              static_cast<std::uint32_t>(lie.by)));
        } else {
            feed::store_be<std::uint64_t>(
                p, feed::load_be<std::uint64_t>(p) + static_cast<std::uint64_t>(lie.by));
        }
        reseal_index(lying);
        const Reader<TypeParam> reader(lying);
        EXPECT_EQ(reader.status(), OpenStatus::Recovered) << "the index was believed";
        // The blocks are untouched, so everything is still found by walking.
        EXPECT_EQ(reader.ticks(), ticks.size());
        EXPECT_EQ(scan_all(reader).ticks, ticks);
        EXPECT_EQ(reader.good_bytes(), index);
    }
    {
        // An index of some other kind. Its checksum starts after the magic
        // and still matches.
        std::vector<std::byte> other = file;
        std::memcpy(other.data() + index, "IDX2", 4);
        const Reader<TypeParam> reader(other);
        EXPECT_EQ(reader.status(), OpenStatus::Recovered);
        EXPECT_EQ(scan_all(reader).ticks, ticks);
    }
}

// An index that is true of the first two blocks, placed after three.
TYPED_TEST(TickFileTest, AnIndexThatLeavesOutABlockIsNotBelieved) {
    const std::vector<Tick> ticks = market_ticks(34, 240);
    const std::vector<std::byte> three = store_of<TypeParam>(ticks, 80);
    const std::vector<std::byte> two = store_of<TypeParam>(first(ticks, 160), 80);
    ASSERT_EQ(first_bytes(three, index_offset(two)), first_bytes(two, index_offset(two)))
        << "the first two blocks of both stores should be the same bytes";

    std::vector<std::byte> file = first_bytes(three, index_offset(three));
    file.insert(file.end(), two.begin() + static_cast<std::ptrdiff_t>(index_offset(two)),
                two.end() - static_cast<std::ptrdiff_t>(kTrailerSize));
    std::vector<std::byte> trailer(kTrailerSize);
    feed::store_be<std::uint64_t>(trailer.data(), index_offset(three));
    std::memcpy(trailer.data() + 8, "OBEX", 4);
    file.insert(file.end(), trailer.begin(), trailer.end());

    const Reader<TypeParam> reader(file);
    EXPECT_EQ(reader.status(), OpenStatus::Recovered);
    EXPECT_EQ(reader.blocks().size(), 3U);
    EXPECT_EQ(scan_all(reader).ticks, ticks) << "the third block was dropped";
}

// A block built by hand, with a correct checksum over contents that are not
// what its header claims: what a writer with a bug would leave. It cannot be
// told from a good block until it is decoded.
template <class Codec>
std::vector<std::byte> store_with_one_block(const std::vector<Tick>& ticks,
                                            std::uint32_t claimed_count,
                                            bool range_backwards = false,
                                            std::string_view magic = "BLK1") {
    std::vector<std::byte> file;
    { const Writer<Codec> header_only(AppendTicksTo{&file}, 8); }
    std::vector<std::byte> payload;
    Codec codec;
    codec.reset();
    Nanos earliest = ticks.empty() ? 0 : kEndOfTime;
    Nanos latest = 0;
    for (const Tick& tick : ticks) {
        std::vector<std::byte> room(Codec::kMaxTickSize);
        const std::size_t n = codec.encode(tick, room.data());
        payload.insert(payload.end(), room.begin(), room.begin() + static_cast<std::ptrdiff_t>(n));
        earliest = std::min(earliest, tick.timestamp);
        latest = std::max(latest, tick.timestamp);
    }
    if (range_backwards) {
        std::swap(earliest, latest);
    }
    std::vector<std::byte> block(kBlockHeaderSize);
    std::memcpy(block.data(), magic.data(), 4);
    feed::store_be<std::uint32_t>(block.data() + 8, claimed_count);
    feed::store_be<std::uint32_t>(block.data() + 12, static_cast<std::uint32_t>(payload.size()));
    feed::store_be<std::uint64_t>(block.data() + 16, earliest);
    feed::store_be<std::uint64_t>(block.data() + 24, latest);
    block.insert(block.end(), payload.begin(), payload.end());
    feed::store_be<std::uint32_t>(block.data() + 4,
                                  util::crc32({block.data() + 8, block.size() - 8}));
    file.insert(file.end(), block.begin(), block.end());
    return file;
}

TYPED_TEST(TickFileTest, ABlockThatWasWrittenWronglyIsReportedThoughItsChecksumMatches) {
    const std::vector<Tick> ticks = market_ticks(35, 3);
    {
        // The hand-made block, made honestly, is a good one.
        const std::vector<std::byte> file = store_with_one_block<TypeParam>(ticks, 3);
        const Reader<TypeParam> reader(file);
        ASSERT_EQ(reader.status(), OpenStatus::Recovered);
        ASSERT_EQ(reader.ticks(), 3U);
        const auto scanned = scan_all(reader);
        ASSERT_TRUE(scanned.result.ok);
        ASSERT_EQ(scanned.ticks, ticks);
    }
    {
        // It says four ticks and holds three.
        const std::vector<std::byte> file = store_with_one_block<TypeParam>(ticks, 4);
        const Reader<TypeParam> reader(file);
        ASSERT_EQ(reader.ticks(), 4U) << "nothing but decoding can tell";
        const auto scanned = scan_all(reader);
        EXPECT_FALSE(scanned.result.ok);
        EXPECT_EQ(scanned.result.bad_block, 0U);
        EXPECT_TRUE(starts(ticks, scanned.ticks));
    }
    {
        // It says two ticks and holds three: bytes are left over. With a
        // codec of fixed width the sizes already give it away when the store
        // is opened; with any other, the scan must.
        const std::vector<std::byte> file = store_with_one_block<TypeParam>(ticks, 2);
        const Reader<TypeParam> reader(file);
        const auto scanned = scan_all(reader);
        EXPECT_TRUE(reader.ticks() == 0 || !scanned.result.ok)
            << "a block with bytes left over after its last tick was read as sound";
        EXPECT_TRUE(starts(ticks, scanned.ticks));
    }
}

// Blocks whose headers say something no writer writes. Their checksums are
// right, so only the reader's own sense of what a block can be stands
// between them and a scan.
TYPED_TEST(TickFileTest, ABlockNoWriterWouldProduceIsNotTakenForOne) {
    const auto not_a_block = [](const std::vector<std::byte>& file) {
        const Reader<TypeParam> reader(file);
        return reader.status() == OpenStatus::Recovered && reader.blocks().empty() &&
               reader.ticks() == 0 && reader.good_bytes() == kFileHeaderSize &&
               scan_all(reader).ticks.empty();
    };
    // The store's header says eight ticks to a block.
    const std::vector<Tick> nine = market_ticks(36, 9);
    ASSERT_FALSE(not_a_block(store_with_one_block<TypeParam>(first(nine, 8), 8)));
    EXPECT_TRUE(not_a_block(store_with_one_block<TypeParam>(nine, 9)))
        << "more ticks than the store puts in a block";
    EXPECT_TRUE(not_a_block(store_with_one_block<TypeParam>({}, 0))) << "no ticks at all";
    const std::vector<Tick> three = first(nine, 3);
    ASSERT_NE(three.front().timestamp, three.back().timestamp);
    EXPECT_TRUE(not_a_block(store_with_one_block<TypeParam>(three, 3, true)))
        << "a range of time that ends before it begins";
    // The checksum starts after the magic, so only the magic can say this.
    EXPECT_TRUE(not_a_block(store_with_one_block<TypeParam>(three, 3, false, "BLK2")))
        << "a block of some other kind";
}

// --- Other files ---------------------------------------------------------------------

TYPED_TEST(TickFileTest, AStoreIsReadOnlyWithTheCodecThatWroteIt) {
    using Other = std::conditional_t<std::is_same_v<TypeParam, store::RawCodec>, store::DeltaCodec,
                                     store::RawCodec>;
    const std::vector<std::byte> file = store_of<TypeParam>(market_ticks(41, 100), 16);
    const store::TickReader<Other> reader(file);
    EXPECT_EQ(reader.status(), OpenStatus::WrongCodec);
    EXPECT_FALSE(reader.usable());
    EXPECT_EQ(reader.ticks(), 0U);
    EXPECT_TRUE(reader.blocks().empty());
    bool called = false;
    const ScanResult result = reader.scan([&called](const Tick&) {
        called = true;
        return true;
    });
    EXPECT_FALSE(called);
    EXPECT_EQ(result, ScanResult{});

    // The header says which codec it was, to a tool that knows neither.
    const std::optional<store::FileHeader> header = store::read_header(file);
    ASSERT_TRUE(header.has_value());
    bool right = false;
    EXPECT_TRUE(store::with_codec_id(header->codec, [&right]<class C>(std::type_identity<C>) {
        right = std::is_same_v<C, TypeParam>;
    }));
    EXPECT_TRUE(right);
    right = false;
    EXPECT_TRUE(store::with_codec(TypeParam::kName, [&right]<class C>(std::type_identity<C>) {
        right = std::is_same_v<C, TypeParam>;
    }));
    EXPECT_TRUE(right);
    EXPECT_FALSE(store::with_codec_id(999, [](auto) { ADD_FAILURE() << "there is no codec 999"; }));
    EXPECT_FALSE(
        store::with_codec("gzip", [](auto) { ADD_FAILURE() << "there is no such codec"; }));
    int codecs = 0;
    store::for_each_codec([&codecs](auto) { ++codecs; });
    EXPECT_EQ(codecs, 2);
}

TYPED_TEST(TickFileTest, WhatIsNotATickStoreIsRefused) {
    const std::vector<std::byte> good = store_of<TypeParam>(market_ticks(42, 20), 8);
    const auto refused = [](const std::vector<std::byte>& bytes) {
        const Reader<TypeParam> reader(bytes);
        return reader.status() == OpenStatus::BadHeader && !reader.usable() &&
               reader.ticks() == 0 && reader.blocks().empty() &&
               scan_all(reader).result == ScanResult{} && !store::read_header(bytes).has_value();
    };
    const auto with_bytes = [&good](std::size_t at, std::initializer_list<unsigned> bytes) {
        std::vector<std::byte> out = good;
        for (const unsigned b : bytes) {
            out[at++] = static_cast<std::byte>(b);
        }
        return out;
    };
    ASSERT_FALSE(refused(good));
    EXPECT_TRUE(refused({}));
    EXPECT_TRUE(refused(first_bytes(good, 1)));
    EXPECT_TRUE(refused(first_bytes(good, kFileHeaderSize - 1)));
    EXPECT_TRUE(refused(with_bytes(0, {'O', 'B', 'E', 'X'}))) << "another magic";
    EXPECT_TRUE(refused(with_bytes(4, {0, 2}))) << "a later version";
    EXPECT_TRUE(refused(with_bytes(4, {0, 0}))) << "version zero";
    EXPECT_TRUE(refused(with_bytes(8, {0, 0, 0, 0}))) << "no ticks to a block";
    EXPECT_TRUE(refused(with_bytes(8, {0, 1, 0, 1}))) << "more ticks to a block than allowed";
    EXPECT_TRUE(refused(with_bytes(15, {1}))) << "the field that must be zero";
    // The largest block size is a header that can be read.
    EXPECT_TRUE(store::read_header(with_bytes(8, {0, 1, 0, 0})).has_value());
    // An ITCH file is not a tick store.
    EXPECT_TRUE(refused(test::engine_feed(test::busy_config(43), 200)));
}

// --- From a book ---------------------------------------------------------------------

TYPED_TEST(TickFileTest, ARecorderStoresWhatABookPublishes) {
    const gen::FlowConfig cfg = test::busy_config(51);
    const std::vector<std::byte> feed_bytes = test::engine_feed(cfg, 20'000);

    using Recorder = store::TickRecorder<TypeParam, AppendTicksTo>;
    using Books = book::BookManager<book::OrderStore, book::PriceLevels, Recorder>;
    std::vector<std::byte> file;
    Writer<TypeParam> writer(AppendTicksTo{&file}, 512);
    const auto books = std::make_unique<Books>(book::OrderStore{}, Recorder(writer));
    feed::ItchParser parser(*books);
    ASSERT_TRUE(parser.parse(feed_bytes).ok());
    writer.finish();

    // The same feed through a book that only keeps a list.
    const std::vector<Tick> expected = test::engine_ticks(cfg, 20'000);
    ASSERT_GT(expected.size(), 5'000U);
    EXPECT_EQ(writer.ticks(), expected.size());
    const Reader<TypeParam> reader(file);
    ASSERT_EQ(reader.status(), OpenStatus::Complete);
    const auto scanned = scan_all(reader);
    EXPECT_TRUE(scanned.result.ok);
    ASSERT_EQ(scanned.ticks.size(), expected.size());
    EXPECT_TRUE(starts(expected, scanned.ticks));
}

}  // namespace
