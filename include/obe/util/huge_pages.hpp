#pragma once

#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <utility>

// Memory for one large table, with a request for transparent huge pages
// (Linux only). Phase 4, experiment 8.
//
// Why it can matter. A CPU translates virtual addresses through the TLB, a
// small cache of page mappings. With 4 KB pages a table of a few hundred
// megabytes spans far more pages than the TLB holds, so random lookups miss
// the TLB as well as the data cache, and each miss is a page-table walk. A
// 2 MB page covers 512 times as much memory per TLB entry.
//
// What this does. It maps anonymous memory aligned to 2 MB and calls
// madvise(MADV_HUGEPAGE), which asks the kernel to back the range with huge
// pages. It is a request, not a guarantee: the kernel grants it only if
// transparent huge pages are enabled ("always" or "madvise" in
// /sys/kernel/mm/transparent_hugepage/enabled) and it can find free 2 MB
// blocks. huge_bytes() reports how much was actually granted. Record that
// number in the experiment: a run that asked for huge pages and got none is
// not a huge-page run.
//
// The memory comes back zero-filled, as all fresh anonymous mappings do.

namespace obe::util {

class HugePageBuffer {
 public:
    static constexpr std::size_t kHugePageSize = std::size_t{2} << 20;

    HugePageBuffer() noexcept = default;

    // Maps at least `bytes`, rounded up to a multiple of 2 MB. Throws
    // std::bad_alloc if the mapping fails. `request_huge = false` gives the
    // same mapping with an explicit request for ordinary pages, for the
    // "before" half of the experiment.
    explicit HugePageBuffer(std::size_t bytes, bool request_huge = true) {
        if (bytes == 0) {
            return;
        }
        size_ = (bytes + kHugePageSize - 1) / kHugePageSize * kHugePageSize;
        // Over-map by one huge page, then trim, so the kept range starts on a
        // 2 MB boundary. An unaligned range cannot be backed by huge pages.
        const std::size_t mapped = size_ + kHugePageSize;
        void* raw =
            ::mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) {
            size_ = 0;
            throw std::bad_alloc();
        }
        auto* base = static_cast<std::byte*>(raw);
        const auto address = reinterpret_cast<std::size_t>(base);
        const std::size_t lead = (kHugePageSize - address % kHugePageSize) % kHugePageSize;
        if (lead != 0) {
            ::munmap(base, lead);
        }
        const std::size_t tail = mapped - lead - size_;
        if (tail != 0) {
            ::munmap(base + lead + size_, tail);
        }
        data_ = base + lead;
        // Advice only. If it fails the buffer still works, on ordinary pages.
        advice_accepted_ =
            ::madvise(data_, size_, request_huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE) == 0;
        requested_huge_ = request_huge;
    }

    HugePageBuffer(const HugePageBuffer&) = delete;
    HugePageBuffer& operator=(const HugePageBuffer&) = delete;

    HugePageBuffer(HugePageBuffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          requested_huge_(other.requested_huge_),
          advice_accepted_(other.advice_accepted_) {}

    HugePageBuffer& operator=(HugePageBuffer&& other) noexcept {
        if (this != &other) {
            release();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            requested_huge_ = other.requested_huge_;
            advice_accepted_ = other.advice_accepted_;
        }
        return *this;
    }

    ~HugePageBuffer() { release(); }

    [[nodiscard]] std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool requested_huge() const noexcept { return requested_huge_; }

    // True if the kernel accepted the madvise call. It can accept the advice
    // and still not provide huge pages; huge_bytes() is the real answer.
    [[nodiscard]] bool advice_accepted() const noexcept { return advice_accepted_; }

    // Bytes of this buffer currently backed by huge pages, read from
    // /proc/self/smaps. Pages are only backed once they have been touched, so
    // call this after the table has been used. Returns 0 if the information is
    // not available. It can also read 0 when the kernel has merged this
    // mapping with a neighbouring one; process_huge_bytes() is the cross-check.
    [[nodiscard]] std::size_t huge_bytes() const {
        if (data_ == nullptr) {
            return 0;
        }
        std::ifstream smaps("/proc/self/smaps");
        const auto begin = reinterpret_cast<std::size_t>(data_);
        const std::size_t end = begin + size_;
        std::string line;
        bool inside = false;
        std::size_t total_kb = 0;
        while (std::getline(smaps, line)) {
            std::size_t lo = 0;
            std::size_t hi = 0;
            // A mapping header looks like "7f12a0000000-7f12c0000000 rw-p ...".
            if (parse_range(line, lo, hi)) {
                inside = lo >= begin && hi <= end;
                continue;
            }
            if (inside && line.rfind("AnonHugePages:", 0) == 0) {
                total_kb += parse_kb(line);
            }
        }
        return total_kb * 1024;
    }

 private:
    void release() noexcept {
        if (data_ != nullptr) {
            ::munmap(data_, size_);
            data_ = nullptr;
            size_ = 0;
        }
    }

    static bool parse_range(const std::string& line, std::size_t& lo, std::size_t& hi) {
        const std::size_t dash = line.find('-');
        const std::size_t space = line.find(' ');
        if (dash == std::string::npos || space == std::string::npos || dash > space) {
            return false;
        }
        char* stop = nullptr;
        errno = 0;
        lo = std::strtoull(line.c_str(), &stop, 16);
        if (errno != 0 || stop != line.c_str() + dash) {
            return false;
        }
        hi = std::strtoull(line.c_str() + dash + 1, &stop, 16);
        return errno == 0 && stop == line.c_str() + space;
    }

    static std::size_t parse_kb(const std::string& line) {
        const std::size_t colon = line.find(':');
        return colon == std::string::npos ? 0
                                          : std::strtoull(line.c_str() + colon + 1, nullptr, 10);
    }

    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    bool requested_huge_ = false;
    bool advice_accepted_ = false;
};

// Anonymous memory of the whole process currently backed by huge pages, from
// /proc/self/smaps_rollup. 0 if it cannot be read.
[[nodiscard]] inline std::size_t process_huge_bytes() {
    std::ifstream in("/proc/self/smaps_rollup");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("AnonHugePages:", 0) == 0) {
            return std::strtoull(line.c_str() + sizeof("AnonHugePages:") - 1, nullptr, 10) * 1024;
        }
    }
    return 0;
}

// The system-wide transparent huge page setting, e.g. "always [madvise] never"
// with the active choice in brackets. Empty if it cannot be read.
[[nodiscard]] inline std::string transparent_huge_page_setting() {
    std::ifstream in("/sys/kernel/mm/transparent_hugepage/enabled");
    std::string line;
    if (in) {
        std::getline(in, line);
    }
    return line;
}

}  // namespace obe::util
