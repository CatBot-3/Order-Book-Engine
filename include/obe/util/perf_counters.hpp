#pragma once

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

// Hardware performance counters for one region of code (Linux only).
//
// `perf stat ./program` counts the whole process, which for a benchmark
// includes loading the file and building the report. These counters are
// switched on and off around the timed loop instead, so "cache misses per
// message" divides by exactly the messages processed while counting.
//
// They are the same kernel counters `perf stat` reads (perf_event_open). They
// are often unavailable: inside most virtual machines and containers, under
// WSL2, or when /proc/sys/kernel/perf_event_paranoid is above 2. Each counter
// opens independently and reports itself unavailable rather than failing, so a
// benchmark still runs and says plainly what it could not measure.

namespace obe::util {

class PerfCounters {
 public:
    enum Counter : std::size_t { Cycles, Instructions, CacheMisses, BranchMisses, kCount };

    struct Sample {
        std::array<std::uint64_t, kCount> value{};
        std::array<bool, kCount> valid{};
    };

    static constexpr std::array<const char*, kCount> kNames{"cycles", "instructions",
                                                            "cache-misses", "branch-misses"};

    PerfCounters() {
        static constexpr std::array<std::uint64_t, kCount> configs{
            PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_CACHE_MISSES,
            PERF_COUNT_HW_BRANCH_MISSES};
        for (std::size_t i = 0; i < kCount; ++i) {
            perf_event_attr attr{};
            attr.type = PERF_TYPE_HARDWARE;
            attr.size = sizeof(attr);
            attr.config = configs[i];
            attr.disabled = 1;
            attr.exclude_kernel = 1;
            attr.exclude_hv = 1;
            // This thread, any CPU.
            fds_[i] = static_cast<int>(::syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
            if (fds_[i] < 0 && error_.empty()) {
                error_ = std::strerror(errno);
            }
        }
    }

    PerfCounters(const PerfCounters&) = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;

    ~PerfCounters() {
        for (const int fd : fds_) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }

    // True if at least one counter opened.
    [[nodiscard]] bool any_available() const noexcept {
        for (const int fd : fds_) {
            if (fd >= 0) {
                return true;
            }
        }
        return false;
    }

    // Why the first unavailable counter failed to open. Empty if all opened.
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    // Zero the counters and start counting.
    void start() noexcept {
        for (const int fd : fds_) {
            if (fd >= 0) {
                ::ioctl(fd, PERF_EVENT_IOC_RESET, 0);
                ::ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
            }
        }
    }

    // Stop counting and return what was counted since start().
    [[nodiscard]] Sample stop() noexcept {
        for (const int fd : fds_) {
            if (fd >= 0) {
                ::ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
            }
        }
        Sample out;
        for (std::size_t i = 0; i < kCount; ++i) {
            if (fds_[i] < 0) {
                continue;
            }
            std::uint64_t value = 0;
            if (::read(fds_[i], &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
                out.value[i] = value;
                out.valid[i] = true;
            }
        }
        return out;
    }

 private:
    std::array<int, kCount> fds_{-1, -1, -1, -1};
    std::string error_;
};

}  // namespace obe::util
