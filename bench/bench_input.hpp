#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

// The input of a file-replay benchmark, fully in memory before any clock
// starts. Shared by replay_bench and pipeline_bench.

namespace obe::bench {

// A private, anonymous copy of the file. Unlike a file mapping, these pages
// cannot be dropped from the page cache and read back from disk in the middle
// of a timed run.
class MemoryCopy {
 public:
    explicit MemoryCopy(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error("cannot open '" + path + "': " + std::strerror(errno));
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            throw std::runtime_error("'" + path + "' is not a readable regular file");
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ == 0) {
            ::close(fd);
            return;
        }
        void* p =
            ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            const int err = errno;
            ::close(fd);
            throw std::runtime_error("cannot allocate " + std::to_string(size_) +
                                     " bytes: " + std::strerror(err) + ". Try --no-copy.");
        }
        data_ = static_cast<std::byte*>(p);
        std::size_t done = 0;
        while (done < size_) {
            const ssize_t n = ::read(fd, data_ + done, size_ - done);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                const int err = errno;
                ::close(fd);
                ::munmap(data_, size_);
                data_ = nullptr;
                throw std::runtime_error("cannot read '" + path +
                                         "': " + (n == 0 ? "file shrank" : std::strerror(err)));
            }
            done += static_cast<std::size_t>(n);
        }
        ::close(fd);
    }
    MemoryCopy(const MemoryCopy&) = delete;
    MemoryCopy& operator=(const MemoryCopy&) = delete;
    ~MemoryCopy() {
        if (data_ != nullptr) {
            ::munmap(data_, size_);
        }
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

 private:
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

// Read one byte from every page so that a file mapping is resident before the
// clock starts. Returns a value that depends on what was read, so the loop
// cannot be optimized away.
[[nodiscard]] inline std::uint64_t touch_pages(std::span<const std::byte> buf) {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < buf.size(); i += 4096) {
        sum += static_cast<std::uint64_t>(buf[i]);
    }
    return sum;
}

}  // namespace obe::bench
