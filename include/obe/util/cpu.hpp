#pragma once

#include <sched.h>

#include <fstream>
#include <string>

// Thread placement and a few facts about the machine (Linux only).

namespace obe::util {

// Restrict the calling thread to one CPU. Returns false if the kernel refused,
// for example because that CPU is not in the process's allowed set.
//
// Why pin a benchmark: when the scheduler moves a thread to another core, the
// thread arrives to cold caches and a cold branch predictor. On an unpinned
// run those migrations show up as latency spikes that are not the code's.
[[nodiscard]] inline bool pin_to_cpu(int cpu) noexcept {
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu), &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

// The CPU the calling thread is running on right now, or -1.
[[nodiscard]] inline int current_cpu() noexcept {
    return sched_getcpu();
}

// The first line of a file, or "" if it cannot be read.
[[nodiscard]] inline std::string read_first_line(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    if (in) {
        std::getline(in, line);
    }
    return line;
}

// The value of the first "key : value" line in a /proc-style file whose key
// starts with `key`, or "".
[[nodiscard]] inline std::string read_proc_value(const std::string& path, const std::string& key) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, key.size(), key) != 0) {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::size_t start = colon + 1;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        return line.substr(start);
    }
    return {};
}

}  // namespace obe::util
