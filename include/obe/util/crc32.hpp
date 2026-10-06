#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// CRC-32, the checksum of Ethernet, zip and PNG (polynomial 0x04C11DB7,
// reflected, so the table is built from 0xEDB88320).
//
// What it is for here: telling a journal record that was written completely
// from one that was cut off or damaged. A CRC is the right tool for that and
// the wrong one for anything adversarial. It is guaranteed to catch every
// single-bit error and every burst of errors up to 32 bits long, and it
// misses a random larger corruption about once in four billion. It offers no
// protection against somebody who changes the data on purpose and recomputes
// it.
//
// Table-driven, one byte at a time: one table lookup, one shift and two XORs
// per byte. Hardware CRC instructions and slicing-by-eight are several times
// faster; a journal record is a few dozen bytes, and the write that follows
// costs thousands of times more than its checksum.

namespace obe::util {

namespace detail {

constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32Table = make_crc32_table();

}  // namespace detail

// The CRC-32 of `data`. To checksum something in pieces, pass the result for
// the earlier pieces as `so_far`: crc32(b, crc32(a)) is the CRC of a followed
// by b.
[[nodiscard]] constexpr std::uint32_t crc32(std::span<const std::byte> data,
                                            std::uint32_t so_far = 0) noexcept {
    std::uint32_t c = ~so_far;
    for (const std::byte b : data) {
        c = detail::kCrc32Table[(c ^ static_cast<std::uint32_t>(b)) & 0xFFU] ^ (c >> 8);
    }
    return ~c;
}

}  // namespace obe::util
