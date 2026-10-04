#include "obe/feed/endian.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

using obe::feed::byteswap;
using obe::feed::load_be;
using obe::feed::load_be48;
using obe::feed::store_be;
using obe::feed::store_be48;

constexpr std::array<std::byte, 9> kBytes{std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
                                          std::byte{0x03}, std::byte{0x04}, std::byte{0x05},
                                          std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};

TEST(Endian, ByteswapReversesBytes) {
    EXPECT_EQ(byteswap<std::uint8_t>(0xab), 0xab);
    EXPECT_EQ(byteswap<std::uint16_t>(0x0102), 0x0201);
    EXPECT_EQ(byteswap<std::uint32_t>(0x01020304U), 0x04030201U);
    EXPECT_EQ(byteswap<std::uint64_t>(0x0102030405060708ULL), 0x0807060504030201ULL);
    static_assert(byteswap<std::uint32_t>(0x01020304U) == 0x04030201U);
}

TEST(Endian, LoadsBigEndianAtOddOffsets) {
    // Offset 1 is misaligned for every type wider than a byte. A cast-based
    // load would be undefined behaviour here; UBSan would flag it.
    const std::byte* p = kBytes.data() + 1;
    EXPECT_EQ(load_be<std::uint8_t>(p), 0x01);
    EXPECT_EQ(load_be<std::uint16_t>(p), 0x0102);
    EXPECT_EQ(load_be<std::uint32_t>(p), 0x01020304U);
    EXPECT_EQ(load_be<std::uint64_t>(p), 0x0102030405060708ULL);
}

TEST(Endian, Loads48BitTimestamp) {
    EXPECT_EQ(load_be48(kBytes.data() + 1), 0x010203040506ULL);
}

TEST(Endian, Load48DoesNotReadPastSixBytes) {
    // The buffer is exactly six bytes on the heap. If load_be48 read eight
    // bytes and masked, AddressSanitizer would report a heap overflow.
    const auto exact = std::make_unique<std::byte[]>(6);
    for (std::size_t i = 0; i < 6; ++i) {
        exact[i] = std::byte{0xff};
    }
    EXPECT_EQ(load_be48(exact.get()), 0xffffffffffffULL);
}

TEST(Endian, Load48HandlesHighBitsOfEachPart) {
    // 86,399.999999999 s: the last nanosecond of a day, the largest real value.
    const std::uint64_t last_ns_of_day = 86'399'999'999'999ULL;
    std::array<std::byte, 6> buf{};
    store_be48(buf.data(), last_ns_of_day);
    EXPECT_EQ(load_be48(buf.data()), last_ns_of_day);
    EXPECT_EQ(buf[0], std::byte{0x4e});  // 0x4e94914effff
    EXPECT_EQ(buf[5], std::byte{0xff});
}

TEST(Endian, StoreWritesBigEndian) {
    std::array<std::byte, 8> buf{};
    store_be<std::uint32_t>(buf.data() + 1, 0xa1b2c3d4U);
    EXPECT_EQ(buf[0], std::byte{0x00});
    EXPECT_EQ(buf[1], std::byte{0xa1});
    EXPECT_EQ(buf[2], std::byte{0xb2});
    EXPECT_EQ(buf[3], std::byte{0xc3});
    EXPECT_EQ(buf[4], std::byte{0xd4});
    EXPECT_EQ(buf[5], std::byte{0x00});
}

TEST(Endian, Store48DropsBitsAbove48AndTouchesOnlySixBytes) {
    std::array<std::byte, 8> buf{};
    buf[6] = std::byte{0x77};
    buf[7] = std::byte{0x77};
    store_be48(buf.data(), 0xffff'0102'0304'0506ULL);
    EXPECT_EQ(load_be48(buf.data()), 0x010203040506ULL);
    EXPECT_EQ(buf[6], std::byte{0x77});
    EXPECT_EQ(buf[7], std::byte{0x77});
}

TEST(Endian, StoreThenLoadRoundTrips) {
    std::array<std::byte, 16> buf{};
    store_be<std::uint16_t>(buf.data() + 3, 0xbeef);
    EXPECT_EQ(load_be<std::uint16_t>(buf.data() + 3), 0xbeef);
    store_be<std::uint64_t>(buf.data() + 5, 0x1122334455667788ULL);
    EXPECT_EQ(load_be<std::uint64_t>(buf.data() + 5), 0x1122334455667788ULL);
}

}  // namespace
