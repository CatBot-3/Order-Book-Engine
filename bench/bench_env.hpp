#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "obe/util/cpu.hpp"
#include "obe/util/format.hpp"
#include "obe/util/huge_pages.hpp"
#include "obe/util/latency_histogram.hpp"

// What every benchmark in this directory reports the same way: the machine and
// the build it ran on, the cost of the timer itself, and how a set of runs is
// reduced to a median and a range. docs/benchmark-method.md explains why each
// of these is in a report.

#ifndef OBE_BUILD_FLAGS
#define OBE_BUILD_FLAGS "unknown"
#endif
#ifndef OBE_BUILD_TYPE
#define OBE_BUILD_TYPE "unknown"
#endif

namespace obe::bench {

// The latency loop with the message taken out: what a sample would read if
// processing took no time at all.
template <class Clock>
[[nodiscard]] std::unique_ptr<util::LatencyHistogram> timer_floor(std::uint64_t samples) {
    auto hist = std::make_unique<util::LatencyHistogram>();
    std::uint64_t previous = Clock::now();
    for (std::uint64_t i = 0; i < samples; ++i) {
        const std::uint64_t now = Clock::now();
        hist->record(now - previous);
        previous = now;
    }
    return hist;
}

struct Summary {
    double median = 0;
    double min = 0;
    double max = 0;
    // (max - min) / median, in percent.
    [[nodiscard]] double spread_percent() const {
        return median > 0 ? 100.0 * (max - min) / median : 0.0;
    }
};

// The median, minimum and maximum of get(run) over the runs.
template <class Run, class Get>
[[nodiscard]] Summary summarize(const std::vector<Run>& runs, Get get) {
    std::vector<double> values;
    values.reserve(runs.size());
    for (const Run& r : runs) {
        values.push_back(get(r));
    }
    std::sort(values.begin(), values.end());
    Summary s;
    if (values.empty()) {
        return s;
    }
    const std::size_t n = values.size();
    s.median = n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
    s.min = values.front();
    s.max = values.back();
    return s;
}

struct Environment {
    std::string cpu_model;
    unsigned logical_cpus = 0;
    std::string memory;
    std::string kernel;
    std::string governor;
    std::string compiler;
    std::string huge_page_setting;
    bool hypervisor = false;
    bool wsl = false;
    bool pinned = false;
    int cpu = -1;
};

[[nodiscard]] inline std::string compiler_string() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

[[nodiscard]] inline bool contains_ci(std::string haystack, std::string_view needle) {
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(needle) != std::string::npos;
}

// `cpu` is the CPU the thread was pinned to; it is ignored if `pinned` is false.
[[nodiscard]] inline Environment gather_environment(int cpu_requested, bool pinned) {
    Environment env;
    env.cpu_model = util::read_proc_value("/proc/cpuinfo", "model name");
    env.logical_cpus = std::thread::hardware_concurrency();
    env.memory = util::read_proc_value("/proc/meminfo", "MemTotal");
    env.kernel = util::read_first_line("/proc/sys/kernel/osrelease");
    env.wsl = contains_ci(env.kernel, "microsoft");
    env.hypervisor =
        (" " + util::read_proc_value("/proc/cpuinfo", "flags") + " ").find(" hypervisor ") !=
        std::string::npos;
    const int cpu = pinned ? cpu_requested : 0;
    env.governor = util::read_first_line("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                                         "/cpufreq/scaling_governor");
    env.compiler = compiler_string();
    env.huge_page_setting = util::transparent_huge_page_setting();
    env.pinned = pinned;
    env.cpu = pinned ? cpu_requested : util::current_cpu();
    return env;
}

[[nodiscard]] inline bool is_instrumented_build() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(undefined_behavior_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

[[nodiscard]] inline bool is_release_build() {
#ifdef NDEBUG
    return true;
#else
    return false;
#endif
}

[[nodiscard]] inline std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// The machine and build lines of a text report.
inline void print_environment(const Environment& env, std::size_t huge_bytes) {
    std::printf("  cpu         %s\n", env.cpu_model.c_str());
    std::printf("              %u logical CPUs, %s RAM%s\n", env.logical_cpus, env.memory.c_str(),
                env.hypervisor ? ", running under a hypervisor" : "");
    if (env.pinned) {
        std::printf("  thread      pinned to CPU %d\n", env.cpu);
    } else {
        std::printf("  thread      NOT pinned (started on CPU %d)\n", env.cpu);
    }
    std::printf("  governor    %s\n", env.governor.empty() ? "unknown" : env.governor.c_str());
    std::printf("  huge pages  setting: %s; in use by this process after the runs: %s bytes\n",
                env.huge_page_setting.empty() ? "unknown" : env.huge_page_setting.c_str(),
                util::with_commas(huge_bytes).c_str());
    std::printf("  kernel      %s%s\n", env.kernel.c_str(), env.wsl ? "  (WSL)" : "");
    std::printf("  compiler    %s\n", env.compiler.c_str());
    std::printf("  build       %s: %s\n", OBE_BUILD_TYPE, OBE_BUILD_FLAGS);
}

// The same facts as JSON members, each line ending in a comma.
inline void write_environment_json(std::FILE* f, const Environment& env, std::size_t huge_bytes) {
    std::fprintf(f, "  \"cpu_model\": \"%s\",\n", json_escape(env.cpu_model).c_str());
    std::fprintf(f, "  \"logical_cpus\": %u,\n", env.logical_cpus);
    std::fprintf(f, "  \"memory\": \"%s\",\n", json_escape(env.memory).c_str());
    std::fprintf(f, "  \"kernel\": \"%s\",\n", json_escape(env.kernel).c_str());
    std::fprintf(f, "  \"hypervisor\": %s,\n", env.hypervisor ? "true" : "false");
    std::fprintf(f, "  \"wsl\": %s,\n", env.wsl ? "true" : "false");
    std::fprintf(f, "  \"pinned_cpu\": %d,\n", env.pinned ? env.cpu : -1);
    std::fprintf(f, "  \"governor\": \"%s\",\n", json_escape(env.governor).c_str());
    std::fprintf(f, "  \"huge_page_setting\": \"%s\",\n",
                 json_escape(env.huge_page_setting).c_str());
    std::fprintf(f, "  \"huge_bytes\": %zu,\n", huge_bytes);
    std::fprintf(f, "  \"compiler\": \"%s\",\n", json_escape(env.compiler).c_str());
    std::fprintf(f, "  \"build_type\": \"%s\",\n", json_escape(OBE_BUILD_TYPE).c_str());
    std::fprintf(f, "  \"build_flags\": \"%s\",\n", json_escape(OBE_BUILD_FLAGS).c_str());
}

}  // namespace obe::bench
