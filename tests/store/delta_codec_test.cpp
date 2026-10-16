#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "obe/store/delta_codec.hpp"

// What only DeltaCodec has to answer for: its own format. The contract tests
// (tick_codec_test.cpp) cover losslessness and hostile bytes in general; these
// pin the rules of design point 8 in delta_codec.hpp, byte by byte. Each
// refused input differs from an accepted one in a single flag, so nothing but
// that flag can be the reason.
//
// The format: one flags byte, the time since the last tick as a zigzag varint,
// the locate in two bytes unless the same-security bit (16) is set, then the
// changed fields. Bits 32 and 64 say a price changed by whole cents (only with
// that price's bit, 1 or 4), bit 128 that sizes changed by round lots (only
// with a size bit, 2 or 8).

namespace {

using namespace obe;
using store::DeltaCodec;
using store::Tick;

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

std::size_t decode(DeltaCodec& codec, const std::vector<std::byte>& in, Tick& tick) {
    return codec.decode(in.data(), in.data() + in.size(), tick);
}

TEST(DeltaCodecFormat, AFirstTickWithNothingChangedIsFourBytes) {
    DeltaCodec codec;
    Tick tick{};
    // No field changed, time 0 after the start of the block, locate 1.
    ASSERT_EQ(decode(codec, bytes({0x00, 0x00, 0x00, 0x01}), tick), 4U);
    EXPECT_EQ(tick, (Tick{.locate = 1, .timestamp = 0, .bbo = {}}));
}

TEST(DeltaCodecFormat, AScaleBitWithoutItsFieldIsRefused) {
    for (const unsigned flag : {0x20U, 0x40U, 0x80U}) {
        SCOPED_TRACE(flag);
        DeltaCodec codec;
        Tick tick{};
        EXPECT_EQ(decode(codec, bytes({flag, 0x00, 0x00, 0x01}), tick), 0U)
            << "the encoder never sets a cents or lots bit for a field that did not change";
    }
    // With its field it is fine: a bid price up one cent, a bid size up one
    // round lot (zigzag 2 is +1).
    {
        DeltaCodec codec;
        Tick tick{};
        ASSERT_EQ(decode(codec, bytes({0x21, 0x00, 0x00, 0x01, 0x02}), tick), 5U);
        EXPECT_EQ(tick.bbo.bid_price, 100U);
    }
    {
        DeltaCodec codec;
        Tick tick{};
        ASSERT_EQ(decode(codec, bytes({0x82, 0x00, 0x00, 0x01, 0x02}), tick), 5U);
        EXPECT_EQ(tick.bbo.bid_qty, 100U);
    }
}

TEST(DeltaCodecFormat, TheSameSecurityBitNeedsATickBeforeItInTheBlock) {
    DeltaCodec codec;
    Tick tick{};
    EXPECT_EQ(decode(codec, bytes({0x10, 0x00}), tick), 0U)
        << "nothing came before it, so there is no security for it to repeat";
    ASSERT_EQ(decode(codec, bytes({0x00, 0x0A, 0x01, 0x02}), tick), 4U);  // time 5
    EXPECT_EQ(tick.locate, 0x0102U);
    ASSERT_EQ(decode(codec, bytes({0x10, 0x02}), tick), 2U) << "now there is one";
    EXPECT_EQ(tick.locate, 0x0102U);
    EXPECT_EQ(tick.timestamp, 6U);
    codec.reset();
    EXPECT_EQ(decode(codec, bytes({0x10, 0x00}), tick), 0U) << "a new block starts with nothing";
}

}  // namespace
