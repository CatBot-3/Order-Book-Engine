#pragma once

#include <cstddef>
#include <cstdint>

// Variable-length integers, and the trick that makes them work for signed
// numbers. These are the two building blocks of most compact formats, and of
// the tick store's codec (obe/store).
//
// VARINT (LEB128, as in Protocol Buffers and DWARF)
//
// A number is cut into groups of seven bits, least significant group first.
// Each group goes into one byte, and the byte's top bit says whether another
// group follows. A value below 128 takes one byte, below 16384 two, and a full
// 64-bit value ten.
//
//   300 = 0b1_0010_1100  ->  0b1010_1100  0b0000_0010
//                              ^ more       ^ last
//
// It pays whenever most values are small, which is what subtracting a nearby
// value (a "delta") is for.
//
// ZIGZAG
//
// A delta can be negative, and a negative number in two's complement has its
// top bits set: -1 is sixty-four ones and would take ten bytes. Zigzag folds
// the number line so that small magnitudes of either sign become small
// unsigned numbers:
//
//    0 -> 0,  -1 -> 1,  1 -> 2,  -2 -> 3,  2 -> 4, ...
//
// ONE ENCODING PER NUMBER
//
// get_varint() accepts only the shortest encoding of a value. "0x80 0x00" is
// two bytes that would decode to zero, and it is refused. A reader that
// accepted it would let two different files mean the same thing, and "decode
// then encode gives back the same bytes" would stop being a property that can
// be tested.

namespace obe::util {

inline constexpr std::size_t kMaxVarintSize = 10;

// Writes `v` at `out` and returns the number of bytes written, 1 to 10.
// Precondition: `out` has room for kMaxVarintSize bytes.
constexpr std::size_t put_varint(std::uint64_t v, std::byte* out) noexcept {
    std::size_t n = 0;
    while (v >= 0x80) {
        out[n++] = static_cast<std::byte>((v & 0x7F) | 0x80);
        v >>= 7;
    }
    out[n++] = static_cast<std::byte>(v);
    return n;
}

// The number of bytes put_varint(v) writes.
[[nodiscard]] constexpr std::size_t varint_size(std::uint64_t v) noexcept {
    std::size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

// Reads one varint from [p, end). Returns the number of bytes it took, or 0
// if there is no whole, shortest-form varint there: the bytes run out, the
// value does not fit in 64 bits, or the encoding has a redundant last byte.
// Never reads at or beyond `end`.
[[nodiscard]] constexpr std::size_t get_varint(const std::byte* p, const std::byte* end,
                                               std::uint64_t& out) noexcept {
    std::uint64_t value = 0;
    for (std::size_t n = 0; n < kMaxVarintSize && p + n < end; ++n) {
        const auto byte = static_cast<std::uint64_t>(p[n]);
        const std::uint64_t group = byte & 0x7F;
        if (n == kMaxVarintSize - 1 && group > 1) {
            return 0;  // the tenth byte holds bit 63 only
        }
        value |= group << (7 * n);
        if ((byte & 0x80) == 0) {
            if (n != 0 && group == 0) {
                return 0;  // a last byte that adds nothing: not the shortest form
            }
            out = value;
            return n + 1;
        }
    }
    return 0;
}

[[nodiscard]] constexpr std::uint64_t zigzag(std::int64_t v) noexcept {
    // The shift of the unsigned value doubles the magnitude; the arithmetic
    // shift of the signed one is all ones for a negative number and all zeros
    // otherwise, so the XOR flips every bit exactly when v is negative.
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}

[[nodiscard]] constexpr std::int64_t unzigzag(std::uint64_t v) noexcept {
    return static_cast<std::int64_t>((v >> 1) ^ (~(v & 1) + 1));
}

}  // namespace obe::util
