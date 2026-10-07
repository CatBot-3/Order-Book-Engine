#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/feed/endian.hpp"
#include "obe/gen/rng.hpp"
#include "obe/store/tick_codec.hpp"
#include "obe/types.hpp"
#include "support/flow_run.hpp"
#include "support/tick_streams.hpp"

// The contract of a tick codec (obe/store/tick_codec.hpp), held against
// whichever codec the build selects. Nothing here knows how a codec writes a
// tick, only what it must give back.

namespace {

using namespace obe;
using store::Tick;
using test::hostile_ticks;
using test::market_ticks;

// What one codec made of a run of ticks from a reset: the bytes, and where
// each tick's bytes end.
struct Encoded {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> ends;
};

template <class Codec>
Encoded encode_all(Codec& codec, const std::vector<Tick>& ticks) {
    Encoded out;
    codec.reset();
    for (const Tick& tick : ticks) {
        // A canary after the room the codec is promised: writing more than
        // kMaxTickSize would trample it.
        std::vector<std::byte> room(Codec::kMaxTickSize + 8, std::byte{0xC5});
        const std::size_t n = codec.encode(tick, room.data());
        EXPECT_GE(n, 1U);
        EXPECT_LE(n, Codec::kMaxTickSize);
        for (std::size_t i = Codec::kMaxTickSize; i < room.size(); ++i) {
            EXPECT_EQ(room[i], std::byte{0xC5}) << "wrote past kMaxTickSize";
        }
        out.bytes.insert(out.bytes.end(), room.begin(),
                         room.begin() + static_cast<std::ptrdiff_t>(n));
        out.ends.push_back(out.bytes.size());
    }
    return out;
}

// Decodes `count` ticks from exactly these bytes. The buffer is the size of
// the data, so a read past the end is a read outside an allocation.
template <class Codec>
::testing::AssertionResult decodes_to(Codec& codec, const std::vector<std::byte>& bytes,
                                      const std::vector<Tick>& expected) {
    codec.reset();
    const std::byte* p = bytes.data();
    const std::byte* const end = bytes.data() + bytes.size();
    for (std::size_t i = 0; i < expected.size(); ++i) {
        Tick tick;
        const std::size_t n = codec.decode(p, end, tick);
        if (n == 0) {
            return ::testing::AssertionFailure() << "tick " << i << " did not decode";
        }
        if (n > static_cast<std::size_t>(end - p)) {
            return ::testing::AssertionFailure() << "tick " << i << " claims more bytes than exist";
        }
        if (!(tick == expected[i])) {
            return ::testing::AssertionFailure()
                   << "tick " << i << " came back as " << ::testing::PrintToString(tick) << ", not "
                   << ::testing::PrintToString(expected[i]);
        }
        p += n;
    }
    if (p != end) {
        return ::testing::AssertionFailure() << (end - p) << " bytes were left over";
    }
    return ::testing::AssertionSuccess();
}

template <class Codec>
::testing::AssertionResult round_trips(const std::vector<Tick>& ticks) {
    Codec writer;
    Codec reader;  // a different object: nothing may pass between them but the bytes
    const Encoded encoded = encode_all(writer, ticks);
    return decodes_to(reader, encoded.bytes, ticks);
}

Tick tick(Nanos t, Locate locate, Price bid, std::uint64_t bid_qty, Price ask,
          std::uint64_t ask_qty) {
    return {locate, t, {bid, bid_qty, ask, ask_qty}};
}

template <class Codec>
class TickCodecTest : public ::testing::Test {};
TYPED_TEST_SUITE(TickCodecTest, test::CodecTypes);

// --- Lossless ----------------------------------------------------------------------

TYPED_TEST(TickCodecTest, AnOrdinaryRunOfTicksComesBack) {
    const Nanos t = 34'200'000'000'000ULL;
    EXPECT_TRUE(round_trips<TypeParam>({
        tick(t, 7, 1'000'000, 300, 1'000'100, 500),
        tick(t + 12'000, 7, 1'000'000, 200, 1'000'100, 500),   // bid size
        tick(t + 12'000, 9, 250'000, 100, 250'300, 100),       // another security, same time
        tick(t + 40'000, 7, 1'000'000, 200, 1'000'200, 800),   // ask moved a cent
        tick(t + 41'000, 7, 999'900, 1'000, 1'000'200, 800),   // bid moved a cent
        tick(t + 90'000, 9, 250'000, 100, 250'300, 100),       // nothing changed
        tick(t + 95'000, 7, 0, 0, 1'000'200, 800),             // the bid side emptied
        tick(t + 99'000, 7, 0, 0, 0, 0),                       // and the ask
        tick(t + 150'000, 7, 1'000'000, 300, 1'000'100, 500),  // and both came back
    }));
}

TYPED_TEST(TickCodecTest, ASingleTickComesBack) {
    EXPECT_TRUE(round_trips<TypeParam>({tick(1, 1, 1, 1, 2, 1)}));
    EXPECT_TRUE(round_trips<TypeParam>({tick(0, 0, 0, 0, 0, 0)}));
}

TYPED_TEST(TickCodecTest, EveryFieldAtItsEdgesComesBack) {
    constexpr auto kTime = std::numeric_limits<Nanos>::max();
    constexpr auto kLocate = std::numeric_limits<Locate>::max();
    constexpr auto kPrice = std::numeric_limits<Price>::max();
    constexpr auto kQty = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(round_trips<TypeParam>({
        tick(kTime, kLocate, kPrice, kQty, kPrice, kQty),
        tick(0, 0, 0, 0, 0, 0),                            // everything falls as far as it can
        tick(kTime, kLocate, kPrice, kQty, kPrice, kQty),  // and rises as far
        tick(kTime, kLocate, 0, kQty, kPrice, 0),
        tick(kTime, kLocate, kPrice, 0, 0, kQty),
        tick(kTime - 1, kLocate, kPrice, 1, 1, kQty - 1),
        tick(std::uint64_t{1} << 63, 1, Price{1} << 31, std::uint64_t{1} << 63, Price{1} << 31,
             std::uint64_t{1} << 63),
        tick((std::uint64_t{1} << 63) - 1, 1, (Price{1} << 31) - 1, (std::uint64_t{1} << 63) - 1,
             (Price{1} << 31) + 1, (std::uint64_t{1} << 63) + 1),
    }));
}

TYPED_TEST(TickCodecTest, TimeMayStandStillOrRunBackwards) {
    EXPECT_TRUE(round_trips<TypeParam>({
        tick(1'000'000, 3, 100, 1, 200, 1),
        tick(1'000'000, 3, 100, 2, 200, 1),
        tick(999'999, 3, 100, 3, 200, 1),
        tick(5, 4, 100, 1, 200, 1),
        tick(0, 3, 100, 4, 200, 1),
        tick(std::numeric_limits<Nanos>::max(), 3, 100, 5, 200, 1),
        tick(0, 3, 100, 6, 200, 1),
    }));
}

TYPED_TEST(TickCodecTest, TheSameTickTwiceIsTwoTicks) {
    const Tick same = tick(77, 5, 1'000, 10, 2'000, 20);
    EXPECT_TRUE(round_trips<TypeParam>({same, same, same}));
}

TYPED_TEST(TickCodecTest, ALongMarketComesBack) {
    for (const std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{3}}) {
        EXPECT_TRUE(round_trips<TypeParam>(market_ticks(seed, 30'000))) << "seed " << seed;
    }
    // Many securities, so that most ticks are the first for a long time.
    EXPECT_TRUE(round_trips<TypeParam>(market_ticks(4, 30'000, 5'000)));
}

TYPED_TEST(TickCodecTest, TicksNoMarketWouldProduceComeBackToo) {
    for (const std::uint64_t seed : {std::uint64_t{11}, std::uint64_t{12}, std::uint64_t{13}}) {
        EXPECT_TRUE(round_trips<TypeParam>(hostile_ticks(seed, 30'000))) << "seed " << seed;
    }
}

TYPED_TEST(TickCodecTest, ARealBooksUpdatesComeBack) {
    const std::vector<Tick> ticks = test::engine_ticks(test::busy_config(5), 20'000);
    ASSERT_GT(ticks.size(), 5'000U);
    EXPECT_TRUE(round_trips<TypeParam>(ticks));
}

// --- reset() -----------------------------------------------------------------------

// The reader starts each block from a reset and has not seen the blocks
// before it. So what a codec writes after a reset must not depend on anything
// it wrote earlier.
TYPED_TEST(TickCodecTest, AfterAResetNothingOfThePastIsLeft) {
    const std::vector<Tick> first = market_ticks(21, 3'000);
    const std::vector<Tick> second = market_ticks(22, 3'000);  // the same securities, other values

    TypeParam used;
    encode_all(used, first);
    const Encoded after_use = encode_all(used, second);  // encode_all resets first

    TypeParam fresh;
    const Encoded from_fresh = encode_all(fresh, second);
    EXPECT_EQ(after_use.bytes, from_fresh.bytes)
        << "a codec that had already written other ticks wrote these differently";

    // And the same on the reading side: a decoder that has read something
    // else reads this block the same.
    TypeParam reader;
    const Encoded other = encode_all(fresh, first);
    ASSERT_TRUE(decodes_to(reader, other.bytes, first));
    EXPECT_TRUE(decodes_to(reader, from_fresh.bytes, second));
}

TYPED_TEST(TickCodecTest, ResettingTwiceOrBeforeAnythingIsHarmless) {
    TypeParam codec;
    codec.reset();
    codec.reset();
    const std::vector<Tick> ticks = market_ticks(23, 500);
    const Encoded once = encode_all(codec, ticks);
    codec.reset();
    codec.reset();
    codec.reset();
    const Encoded again = encode_all(codec, ticks);
    EXPECT_EQ(once.bytes, again.bytes);
}

// Blocks can be any length. Cutting the same ticks into blocks at different
// places must always give ticks back, whatever state the cut discards.
TYPED_TEST(TickCodecTest, ARunCutIntoBlocksAnywhereStillComesBack) {
    const std::vector<Tick> ticks = market_ticks(24, 2'000, 40);
    for (const std::size_t block :
         {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{64}, std::size_t{1'999}}) {
        TypeParam writer;
        TypeParam reader;
        for (std::size_t at = 0; at < ticks.size(); at += block) {
            const std::vector<Tick> piece(
                ticks.begin() + static_cast<std::ptrdiff_t>(at),
                ticks.begin() + static_cast<std::ptrdiff_t>(std::min(at + block, ticks.size())));
            const Encoded encoded = encode_all(writer, piece);
            ASSERT_TRUE(decodes_to(reader, encoded.bytes, piece))
                << "blocks of " << block << ", at tick " << at;
        }
    }
}

// --- Bytes from a file -------------------------------------------------------------

// A tick's bytes say where they end: a stream of them carries no lengths.
// So the first part of a tick is never itself a tick.
TYPED_TEST(TickCodecTest, ATickCutShortIsNotATick) {
    for (const std::vector<Tick>& ticks : {market_ticks(31, 60), hostile_ticks(32, 60)}) {
        TypeParam writer;
        TypeParam reader;  // one for every cut: reset() is all a block gets
        const Encoded encoded = encode_all(writer, ticks);
        for (std::size_t k = 0; k < ticks.size(); ++k) {
            const std::size_t begin = k == 0 ? 0 : encoded.ends[k - 1];
            for (std::size_t cut = begin; cut < encoded.ends[k]; ++cut) {
                // Everything up to tick k decodes; then tick k is cut short.
                const std::vector<std::byte> bytes(
                    encoded.bytes.begin(),
                    encoded.bytes.begin() + static_cast<std::ptrdiff_t>(cut));
                reader.reset();
                const std::byte* p = bytes.data();
                const std::byte* const end = bytes.data() + bytes.size();
                for (std::size_t i = 0; i < k; ++i) {
                    Tick out;
                    const std::size_t n = reader.decode(p, end, out);
                    ASSERT_NE(n, 0U);
                    p += n;
                }
                Tick out;
                ASSERT_EQ(reader.decode(p, end, out), 0U)
                    << "tick " << k << " with " << (cut - begin) << " of "
                    << (encoded.ends[k] - begin) << " bytes";
            }
        }
    }
}

TYPED_TEST(TickCodecTest, NothingToReadIsNotATick) {
    TypeParam codec;
    codec.reset();
    Tick out;
    const std::vector<std::byte> empty;
    EXPECT_EQ(codec.decode(empty.data(), empty.data(), out), 0U);
}

// Whatever the bytes are, decode() stays inside them. This cannot check what
// it returns, only that it returns: AddressSanitizer does the rest. The one
// codec reads every buffer, as a reader's does every block, including the
// blocks after one it gave up on.
TYPED_TEST(TickCodecTest, ArbitraryBytesAreReadWithoutLeavingThem) {
    gen::SplitMix64 rng(41);
    TypeParam codec;
    for (int round = 0; round < 3'000; ++round) {
        std::vector<std::byte> bytes(rng.below(3 * TypeParam::kMaxTickSize));
        for (std::byte& b : bytes) {
            // Half the time bytes with the top bit set, which is what keeps a
            // variable-length reader reading.
            b = static_cast<std::byte>(rng.below(2) == 0 ? rng.below(256) : 0x80 | rng.below(128));
        }
        codec.reset();
        const std::byte* p = bytes.data();
        const std::byte* const end = bytes.data() + bytes.size();
        while (p < end) {
            Tick out;
            const std::size_t n = codec.decode(p, end, out);
            if (n == 0) {
                break;
            }
            ASSERT_LE(n, static_cast<std::size_t>(end - p)) << "claimed bytes that do not exist";
            p += n;
        }
    }
}

// A damaged copy of good bytes is the likelier input: mostly valid, then not.
TYPED_TEST(TickCodecTest, DamagedBytesAreReadWithoutLeavingThem) {
    const std::vector<Tick> ticks = market_ticks(42, 200);
    TypeParam writer;
    const Encoded good = encode_all(writer, ticks);
    gen::SplitMix64 rng(43);
    TypeParam codec;
    for (int round = 0; round < 2'000; ++round) {
        std::vector<std::byte> bytes = good.bytes;
        bytes[rng.below(bytes.size())] ^= static_cast<std::byte>(1 + rng.below(255));
        bytes.resize(1 + rng.below(bytes.size()));
        codec.reset();
        const std::byte* p = bytes.data();
        const std::byte* const end = bytes.data() + bytes.size();
        while (p < end) {
            Tick out;
            const std::size_t n = codec.decode(p, end, out);
            if (n == 0) {
                break;
            }
            ASSERT_LE(n, static_cast<std::size_t>(end - p));
            p += n;
        }
    }
}

}  // namespace

// --- The reference codec's own layout -----------------------------------------------

namespace {

TEST(RawCodec, WritesEveryFieldAtFullWidthWhereTheHeaderSaysItIs) {
    store::RawCodec codec;
    std::vector<std::byte> out(store::RawCodec::kMaxTickSize);
    const Tick t = tick(0x0102030405060708ULL, 0x0A0B, 0x11121314, 0x2122232425262728ULL,
                        0x31323334, 0x4142434445464748ULL);
    ASSERT_EQ(codec.encode(t, out.data()), 34U);
    EXPECT_EQ(feed::load_be<std::uint64_t>(out.data()), 0x0102030405060708ULL);
    EXPECT_EQ(feed::load_be<std::uint16_t>(out.data() + 8), 0x0A0BU);
    EXPECT_EQ(feed::load_be<std::uint32_t>(out.data() + 10), 0x11121314U);
    EXPECT_EQ(feed::load_be<std::uint64_t>(out.data() + 14), 0x2122232425262728ULL);
    EXPECT_EQ(feed::load_be<std::uint32_t>(out.data() + 22), 0x31323334U);
    EXPECT_EQ(feed::load_be<std::uint64_t>(out.data() + 26), 0x4142434445464748ULL);
    // Big-endian: the first byte of the file is the top byte of the time.
    EXPECT_EQ(out[0], std::byte{0x01});
    EXPECT_EQ(out[33], std::byte{0x48});
}

TEST(RawCodec, NeedsNoHistoryToReadATick) {
    const std::vector<Tick> ticks = market_ticks(51, 100);
    store::RawCodec writer;
    const Encoded encoded = encode_all(writer, ticks);
    ASSERT_EQ(encoded.bytes.size(), ticks.size() * 34);
    // Tick 60, read directly, by a codec that has seen nothing.
    store::RawCodec reader;
    Tick out;
    const std::byte* p = encoded.bytes.data() + std::size_t{60} * 34;
    ASSERT_EQ(reader.decode(p, encoded.bytes.data() + encoded.bytes.size(), out), 34U);
    EXPECT_EQ(out, ticks[60]);
}

TEST(RawCodec, HasTheNameAndNumberStoresAreWrittenWith) {
    EXPECT_EQ(store::RawCodec::kName, "raw");
    EXPECT_EQ(store::RawCodec::kId, 1U);
    EXPECT_EQ(store::DeltaCodec::kName, "delta");
    EXPECT_EQ(store::DeltaCodec::kId, 2U);
    // Ids are what a file carries, so two codecs must never share one.
    EXPECT_NE(store::RawCodec::kId, store::DeltaCodec::kId);
}

}  // namespace
