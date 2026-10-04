#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <x86intrin.h>
#define OBE_HAS_TSC 1
#else
#define OBE_HAS_TSC 0
#endif

// Clocks for timing individual messages.
//
// A message takes tens of nanoseconds to process. std::chrono::steady_clock
// costs roughly that much per call on Linux (a vDSO call into clock_gettime),
// so timing every message with it would measure mostly the clock. Reading the
// CPU's timestamp counter directly costs a fraction of that.
//
// Both clocks have the same shape: now() returns ticks, and ns_per_tick()
// converts. A benchmark records raw ticks and converts once at the end.
//
// Caveats of the timestamp counter, all stated in docs/benchmark-method.md:
//   - rdtsc is not a serializing instruction. The CPU may reorder it with
//     neighbouring instructions, which blurs a single interval by a few
//     nanoseconds. It does not bias the sum.
//   - The tick rate is only constant on CPUs with an invariant TSC.
//     tsc_is_invariant() reports that; if it is false, use SteadyClock.
//   - Under a hypervisor (WSL2 included) the counter may be virtualized.

namespace obe::util {

struct SteadyClock {
    static constexpr const char* kName = "steady_clock";

    [[nodiscard]] static std::uint64_t now() noexcept {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }
    [[nodiscard]] static double ns_per_tick() noexcept { return 1.0; }
};

#if OBE_HAS_TSC

// True if the CPU advertises a timestamp counter that ticks at a constant rate
// regardless of frequency scaling and sleep states (CPUID 0x80000007, EDX bit 8).
[[nodiscard]] inline bool tsc_is_invariant() noexcept {
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    if (__get_cpuid(0x80000007U, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    return (edx & (1U << 8)) != 0;
}

struct TscClock {
    static constexpr const char* kName = "rdtsc";

    [[nodiscard]] static std::uint64_t now() noexcept { return __rdtsc(); }

    // Measured once against steady_clock over a 200 ms window and cached. The
    // first call blocks for that long.
    [[nodiscard]] static double ns_per_tick() noexcept {
        static const double value = calibrate();
        return value;
    }

 private:
    [[nodiscard]] static double calibrate() noexcept {
        using steady = std::chrono::steady_clock;
        const auto wall_start = steady::now();
        const std::uint64_t tick_start = __rdtsc();
        while (steady::now() - wall_start < std::chrono::milliseconds(200)) {
        }
        const std::uint64_t tick_end = __rdtsc();
        const auto wall_end = steady::now();
        const auto ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count());
        const auto ticks = static_cast<double>(tick_end - tick_start);
        return ticks > 0 ? ns / ticks : 1.0;
    }
};

#else

[[nodiscard]] inline bool tsc_is_invariant() noexcept {
    return false;
}

// No timestamp counter on this architecture: fall back to the portable clock.
using TscClock = SteadyClock;

#endif

}  // namespace obe::util
