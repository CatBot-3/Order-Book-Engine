#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Big-endian loads and stores for the wire format.
//
// Why memcpy and not a cast to a packed struct: ITCH fields sit at odd offsets,
// so reading one through a std::uint32_t* is a misaligned access, which is
// undefined behaviour even on CPUs that tolerate it. memcpy of a fixed small
// size is defined for any address, and compilers turn it into the same single
// mov (plus a bswap) that the cast would have produced.

namespace obe::feed {

template <std::unsigned_integral T>
[[nodiscard]] constexpr T byteswap(T v) noexcept {
    if constexpr (sizeof(T) == 1) {
        return v;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(v));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(v));
    } else {
        static_assert(sizeof(T) == 8);
        return static_cast<T>(__builtin_bswap64(v));
    }
}

template <std::unsigned_integral T>
[[nodiscard]] inline T load_be(const std::byte* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        v = byteswap(v);
    }
    return v;
}

template <std::unsigned_integral T>
inline void store_be(std::byte* p, T v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        v = byteswap(v);
    }
    std::memcpy(p, &v, sizeof(T));
}

// The ITCH timestamp is six bytes. Reading eight bytes and masking would run
// two bytes past the field, and past the buffer for a message that ends the
// file, so it is assembled from a 2-byte and a 4-byte load instead.
[[nodiscard]] inline std::uint64_t load_be48(const std::byte* p) noexcept {
    const std::uint64_t hi = load_be<std::uint16_t>(p);
    const std::uint64_t lo = load_be<std::uint32_t>(p + 2);
    return (hi << 32) | lo;
}

// Stores the low 48 bits of v. Higher bits are dropped.
inline void store_be48(std::byte* p, std::uint64_t v) noexcept {
    store_be<std::uint16_t>(p, static_cast<std::uint16_t>(v >> 32));
    store_be<std::uint32_t>(p + 2, static_cast<std::uint32_t>(v));
}

}  // namespace obe::feed
