#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// A read-only memory mapping of a whole file (POSIX only).
//
// The parser reads messages in place from this mapping. Nothing is copied, and
// the kernel pages the file in as the parser walks forward.

namespace obe::util {

class MappedFile {
 public:
    enum class Advice { Sequential, None };

    // Throws std::runtime_error with the path and the OS error on failure.
    explicit MappedFile(const std::string& path, Advice advice = Advice::Sequential) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error("cannot open '" + path + "': " + std::strerror(errno));
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            const int err = errno;
            ::close(fd);
            throw std::runtime_error("cannot stat '" + path + "': " + std::strerror(err));
        }
        if (!S_ISREG(st.st_mode)) {
            ::close(fd);
            throw std::runtime_error("'" + path + "' is not a regular file");
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                const int err = errno;
                ::close(fd);
                throw std::runtime_error("cannot map '" + path + "': " + std::strerror(err));
            }
            data_ = static_cast<const std::byte*>(p);
            if (advice == Advice::Sequential) {
                // A hint only. Failure changes speed, not behaviour.
                (void)::madvise(p, size_, MADV_SEQUENTIAL);
            }
        }
        // The mapping keeps the file alive; the descriptor is no longer needed.
        ::close(fd);
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) {
            unmap();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~MappedFile() { unmap(); }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
    void unmap() noexcept {
        if (data_ != nullptr) {
            // The mapping is read-only; dropping const here only satisfies
            // munmap's signature.
            ::munmap(const_cast<void*>(static_cast<const void*>(data_)), size_);
            data_ = nullptr;
            size_ = 0;
        }
    }

    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

// True if the buffer starts with the gzip magic number. The sample files are
// distributed compressed, and mapping one by mistake is an easy error to make.
[[nodiscard]] inline bool looks_gzipped(std::span<const std::byte> buf) noexcept {
    return buf.size() >= 2 && buf[0] == std::byte{0x1f} && buf[1] == std::byte{0x8b};
}

}  // namespace obe::util
