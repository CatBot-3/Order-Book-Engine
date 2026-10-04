#include <gtest/gtest.h>

#include <sched.h>

#include <chrono>
#include <cstdint>
#include <thread>

#include "obe/util/clock.hpp"
#include "obe/util/cpu.hpp"
#include "obe/util/perf_counters.hpp"

// The measurement tools are checked for behaviour, not speed: a clock that runs
// backwards or a conversion factor that is off by a thousand would silently
// ruin every number built on it.

namespace {

using namespace obe::util;

template <class Clock>
void expect_measures_a_sleep() {
    const std::uint64_t start = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::uint64_t end = Clock::now();
    ASSERT_GT(end, start);
    const double ms = static_cast<double>(end - start) * Clock::ns_per_tick() / 1e6;
    // Generous bounds: a loaded CI machine can oversleep, but a clock that is
    // wrong by a factor of ten in either direction still fails.
    EXPECT_GT(ms, 40.0) << Clock::kName;
    EXPECT_LT(ms, 500.0) << Clock::kName;
}

TEST(Clock, SteadyClockMeasuresASleep) {
    expect_measures_a_sleep<SteadyClock>();
}

TEST(Clock, TscClockMeasuresASleep) {
    expect_measures_a_sleep<TscClock>();
}

TEST(Clock, ReadingsNeverGoBackwards) {
    std::uint64_t previous_tsc = TscClock::now();
    std::uint64_t previous_steady = SteadyClock::now();
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t tsc = TscClock::now();
        const std::uint64_t steady = SteadyClock::now();
        ASSERT_GE(tsc, previous_tsc);
        ASSERT_GE(steady, previous_steady);
        previous_tsc = tsc;
        previous_steady = steady;
    }
}

TEST(Clock, TickLengthIsPlausible) {
    EXPECT_EQ(SteadyClock::ns_per_tick(), 1.0);
    // CPUs tick somewhere between 100 MHz and 20 GHz.
    EXPECT_GT(TscClock::ns_per_tick(), 0.05);
    EXPECT_LT(TscClock::ns_per_tick(), 10.0);
    EXPECT_EQ(TscClock::ns_per_tick(), TscClock::ns_per_tick()) << "calibrated once, then cached";
}

TEST(Cpu, PinToAnAllowedCpuSucceedsAndBadCpusAreRefused) {
    cpu_set_t original;
    CPU_ZERO(&original);
    ASSERT_EQ(sched_getaffinity(0, sizeof(original), &original), 0);

    int allowed = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(static_cast<unsigned>(cpu), &original)) {
            allowed = cpu;
            break;
        }
    }
    ASSERT_GE(allowed, 0);

    EXPECT_TRUE(pin_to_cpu(allowed));
    EXPECT_EQ(current_cpu(), allowed);
    EXPECT_FALSE(pin_to_cpu(-1));
    EXPECT_FALSE(pin_to_cpu(CPU_SETSIZE));

    // Leave the test process as it was found.
    EXPECT_EQ(sched_setaffinity(0, sizeof(original), &original), 0);
}

TEST(Cpu, ProcReadersReturnEmptyForMissingFilesAndKeys) {
    EXPECT_EQ(read_first_line("/nonexistent/file"), "");
    EXPECT_EQ(read_proc_value("/nonexistent/file", "model name"), "");
    EXPECT_EQ(read_proc_value("/proc/meminfo", "NoSuchKey"), "");
    EXPECT_FALSE(read_proc_value("/proc/meminfo", "MemTotal").empty());
}

TEST(PerfCounters, WorkWhenAvailableAndDegradeQuietlyWhenNot) {
    // Hardware counters are refused in most virtual machines and containers.
    // Either outcome is acceptable; crashing or reporting garbage is not.
    PerfCounters perf;
    perf.start();
    volatile std::uint64_t sink = 0;
    for (std::uint64_t i = 0; i < 2'000'000; ++i) {
        sink = sink + i;
    }
    const PerfCounters::Sample sample = perf.stop();

    if (!perf.any_available()) {
        EXPECT_FALSE(perf.error().empty()) << "an unavailable counter must say why";
        for (std::size_t i = 0; i < PerfCounters::kCount; ++i) {
            EXPECT_FALSE(sample.valid[i]);
        }
        return;
    }
    if (sample.valid[PerfCounters::Instructions]) {
        // The loop above is at least a few instructions per iteration.
        EXPECT_GT(sample.value[PerfCounters::Instructions], 2'000'000U);
    }
    if (sample.valid[PerfCounters::Cycles]) {
        EXPECT_GT(sample.value[PerfCounters::Cycles], 0U);
    }
}

}  // namespace
