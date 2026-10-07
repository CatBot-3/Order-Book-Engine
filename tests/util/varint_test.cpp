#include "obe/util/varint.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "obe/gen/rng.hpp"

namespace {

using namespace obe;
using util::get_varint;
using util::kMaxVarintSize;
using util::put_varint;
using util::unzigzag;
using util::varint_size;
using util::zigzag;

std::vector<std::byte> encoded(std::uint64_t v) {
    std::array<std::byte, kMaxVarintSize> buf{};
    const std::size_t n = put_varint(v, buf.data());
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)};
}

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// Decodes from a buffer of exactly these bytes, so that a read past the end
// is a read outside an allocation.
std::size_t decode(const std::vector<std::byte>& in, std::uint64_t& out) {
    return get_varint(in.data(), in.data() + in.size(), out);
}

TEST(Varint, SmallNumbersAreOneByteAndEachSevenBitsAddsOne) {
    EXPECT_EQ(encoded(0), bytes({0x00}));
    EXPECT_EQ(encoded(1), bytes({0x01}));
    EXPECT_EQ(encoded(127), bytes({0x7F}));
    EXPECT_EQ(encoded(128), bytes({0x80, 0x01}));
    // The example in the header: 300 = 0b1_0010_1100.
    EXPECT_EQ(encoded(300), bytes({0xAC, 0x02}));
    EXPECT_EQ(encoded(16'383), bytes({0xFF, 0x7F}));
    EXPECT_EQ(encoded(16'384), bytes({0x80, 0x80, 0x01}));
    // The largest value: nine bytes of seven ones, and bit 63 alone in the tenth.
    EXPECT_EQ(encoded(std::numeric_limits<std::uint64_t>::max()),
              bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}));
}

TEST(Varint, TheSizeGrowsAtEveryPowerOfOneHundredAndTwentyEight) {
    for (std::size_t groups = 1; groups <= 9; ++groups) {
        const std::uint64_t first_too_big = std::uint64_t{1} << (7 * groups);
        EXPECT_EQ(varint_size(first_too_big - 1), groups);
        EXPECT_EQ(varint_size(first_too_big), groups + 1);
        EXPECT_EQ(encoded(first_too_big - 1).size(), groups);
        EXPECT_EQ(encoded(first_too_big).size(), groups + 1);
    }
    EXPECT_EQ(varint_size(0), 1U);
    EXPECT_EQ(varint_size(std::numeric_limits<std::uint64_t>::max()), kMaxVarintSize);
}

TEST(Varint, EveryValueComesBackAndSaysHowLongItWas) {
    gen::SplitMix64 rng(7);
    std::vector<std::uint64_t> values{0,
                                      1,
                                      127,
                                      128,
                                      255,
                                      256,
                                      16'383,
                                      16'384,
                                      std::numeric_limits<std::uint64_t>::max(),
                                      std::uint64_t{1} << 63};
    for (int i = 0; i < 20'000; ++i) {
        // Spread over every length: a random number of significant bits.
        values.push_back(rng.next() >> rng.below(64));
    }
    for (const std::uint64_t v : values) {
        const std::vector<std::byte> in = encoded(v);
        ASSERT_EQ(in.size(), varint_size(v));
        std::uint64_t out = ~v;
        ASSERT_EQ(decode(in, out), in.size()) << v;
        ASSERT_EQ(out, v);
    }
}

TEST(Varint, ANumberCutShortIsNotANumber) {
    for (const std::uint64_t v :
         {std::uint64_t{128}, std::uint64_t{1} << 40, std::numeric_limits<std::uint64_t>::max()}) {
        const std::vector<std::byte> whole = encoded(v);
        for (std::size_t n = 0; n < whole.size(); ++n) {
            const std::vector<std::byte> cut(whole.begin(),
                                             whole.begin() + static_cast<std::ptrdiff_t>(n));
            std::uint64_t out = 99;
            EXPECT_EQ(decode(cut, out), 0U) << n << " bytes of " << whole.size();
            EXPECT_EQ(out, 99U) << "a failed read leaves the output alone";
        }
    }
}

TEST(Varint, ReadingStopsAtTheEndOfTheNumber) {
    std::vector<std::byte> in = encoded(300);
    in.push_back(std::byte{0xFF});  // the start of whatever comes next
    in.push_back(std::byte{0xFF});
    std::uint64_t out = 0;
    EXPECT_EQ(decode(in, out), 2U);
    EXPECT_EQ(out, 300U);
}

// One encoding per number: see the header for why.
TEST(Varint, OnlyTheShortestEncodingIsAccepted) {
    std::uint64_t out = 0;
    EXPECT_EQ(decode(bytes({0x80, 0x00}), out), 0U) << "zero, padded to two bytes";
    EXPECT_EQ(decode(bytes({0xAC, 0x82, 0x00}), out), 0U) << "300, padded to three";
    EXPECT_EQ(decode(bytes({0x80, 0x80, 0x80, 0x00}), out), 0U);
    // A zero byte by itself is zero and is fine.
    EXPECT_EQ(decode(bytes({0x00}), out), 1U);
    EXPECT_EQ(out, 0U);
    // And a zero group in the middle is part of the number.
    EXPECT_EQ(decode(bytes({0x80, 0x80, 0x01}), out), 3U);
    EXPECT_EQ(out, 16'384U);
}

TEST(Varint, MoreThanSixtyFourBitsIsRefused) {
    std::uint64_t out = 0;
    // Ten bytes whose tenth carries more than bit 63.
    EXPECT_EQ(decode(bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02}), out), 0U);
    EXPECT_EQ(decode(bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F}), out), 0U);
    // Ten bytes that still say "more follows", with or without an eleventh.
    EXPECT_EQ(decode(bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x81}), out), 0U);
    EXPECT_EQ(
        decode(bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x81, 0x01}), out), 0U);
    // Bit 63 alone is fine.
    EXPECT_EQ(decode(bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01}), out),
              10U);
    EXPECT_EQ(out, std::uint64_t{1} << 63);
}

TEST(Varint, NothingToReadIsNotANumber) {
    std::uint64_t out = 5;
    EXPECT_EQ(decode({}, out), 0U);
    const std::byte one{0x01};
    EXPECT_EQ(get_varint(&one, &one, out), 0U) << "an empty range";
    EXPECT_EQ(out, 5U);
}

TEST(Varint, WorksAtCompileTime) {
    static constexpr auto kSize = [] {
        std::array<std::byte, kMaxVarintSize> buf{};
        return put_varint(300, buf.data());
    }();
    static_assert(kSize == 2);
    static_assert(varint_size(300) == 2);
    static_assert(zigzag(-1) == 1);
    static_assert(unzigzag(1) == -1);
    SUCCEED();
}

TEST(Zigzag, FoldsTheNumberLineSoThatSmallMagnitudesAreSmall) {
    EXPECT_EQ(zigzag(0), 0U);
    EXPECT_EQ(zigzag(-1), 1U);
    EXPECT_EQ(zigzag(1), 2U);
    EXPECT_EQ(zigzag(-2), 3U);
    EXPECT_EQ(zigzag(2), 4U);
    EXPECT_EQ(zigzag(-64), 127U) << "the last negative number that fits one varint byte";
    EXPECT_EQ(zigzag(63), 126U);
    EXPECT_EQ(zigzag(64), 128U);
    EXPECT_EQ(zigzag(std::numeric_limits<std::int64_t>::max()),
              std::numeric_limits<std::uint64_t>::max() - 1);
    EXPECT_EQ(zigzag(std::numeric_limits<std::int64_t>::min()),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(Zigzag, EveryValueComesBack) {
    gen::SplitMix64 rng(9);
    std::vector<std::int64_t> values{0,
                                     1,
                                     -1,
                                     63,
                                     -64,
                                     64,
                                     -65,
                                     std::numeric_limits<std::int64_t>::max(),
                                     std::numeric_limits<std::int64_t>::min()};
    for (int i = 0; i < 20'000; ++i) {
        values.push_back(static_cast<std::int64_t>(rng.next()) >> rng.below(64));
    }
    for (const std::int64_t v : values) {
        ASSERT_EQ(unzigzag(zigzag(v)), v);
    }
    // And the other way: every unsigned value is the image of something.
    for (int i = 0; i < 20'000; ++i) {
        const std::uint64_t u = rng.next() >> rng.below(64);
        ASSERT_EQ(zigzag(unzigzag(u)), u);
    }
}

// What the pair is for: a small difference of either sign in one byte.
TEST(Zigzag, ASmallDifferenceOfEitherSignIsOneVarintByte) {
    for (std::int64_t d = -64; d <= 63; ++d) {
        ASSERT_EQ(varint_size(zigzag(d)), 1U) << d;
    }
    EXPECT_EQ(varint_size(zigzag(64)), 2U);
    EXPECT_EQ(varint_size(zigzag(-65)), 2U);
    // Without zigzag, minus one would be the longest number there is.
    EXPECT_EQ(varint_size(static_cast<std::uint64_t>(std::int64_t{-1})), kMaxVarintSize);
}

}  // namespace
