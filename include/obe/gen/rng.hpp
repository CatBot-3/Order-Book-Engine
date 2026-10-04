#pragma once

#include <cstdint>

// The project's only random number generator.
//
// Everything that generates test data uses this and no std distribution, so
// the same seed gives the same bytes on every platform, compiler and standard
// library. That is what lets a fixture be described by its seed instead of
// being committed as a file.

namespace obe::gen {

// SplitMix64. Small, fast, and identical everywhere.
class SplitMix64 {
 public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    // Uniform enough for test data in [0, n). Precondition: n > 0.
    constexpr std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }

    // Inclusive range. Precondition: lo <= hi.
    constexpr std::uint64_t between(std::uint64_t lo, std::uint64_t hi) noexcept {
        return lo + below(hi - lo + 1);
    }

 private:
    std::uint64_t state_;
};

}  // namespace obe::gen
