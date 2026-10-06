#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// A journal file on disk: something to append to, force onto the disk, and
// cut back to its last good byte after a crash.
//
// WHAT "WRITTEN" MEANS
//
// write() returning does not mean the bytes are on the disk. It means the
// operating system has them, in memory, and will write them out when it gets
// round to it. If the PROCESS dies after that, nothing is lost: the bytes are
// the kernel's now. If the MACHINE loses power, they are gone.
//
// sync() (fdatasync) is what closes that gap. It returns only when the device
// says the data is stored, and it is slow: on the order of a millisecond on a
// consumer SSD, against a microsecond for the write. A journal that syncs
// after every record can record about a thousand requests a second; one that
// never syncs survives a crash of the program and not of the machine. In
// between is group commit: gather the requests of one turn of the event loop,
// write them together, sync once, and only then let their results out. Each
// request waits a little, and the sync is shared by all of them.
//
// journal_bench measures those three points. This class only provides the
// operations; the policy is its caller's.

namespace obe::journal {

class JournalFile {
 public:
    // Opens `path` for appending, creating it if it is not there. Throws
    // std::runtime_error with the path and the system's reason on failure.
    explicit JournalFile(const std::string& path)
        : fd_(::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)), path_(path) {
        if (fd_ < 0) {
            throw std::runtime_error("cannot open journal '" + path + "': " + std::strerror(errno));
        }
    }

    JournalFile(const JournalFile&) = delete;
    JournalFile& operator=(const JournalFile&) = delete;
    JournalFile(JournalFile&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          path_(std::move(other.path_)),
          writes_(other.writes_),
          syncs_(other.syncs_),
          bytes_(other.bytes_) {}
    JournalFile& operator=(JournalFile&&) = delete;

    ~JournalFile() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    // Appends all of `bytes`. A write the system takes only part of is carried
    // on until it is whole; anything else that goes wrong throws, because a
    // journal that silently lost a record is worse than one that stopped.
    void write(std::span<const std::byte> bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t n = ::write(fd_, bytes.data() + done, bytes.size() - done);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("cannot write journal '" + path_ +
                                         "': " + std::strerror(errno));
            }
            done += static_cast<std::size_t>(n);
        }
        ++writes_;
        bytes_ += bytes.size();
    }

    // Returns once the data written so far is on the device.
    void sync() {
        if (::fdatasync(fd_) != 0) {
            throw std::runtime_error("cannot sync journal '" + path_ +
                                     "': " + std::strerror(errno));
        }
        ++syncs_;
    }

    // Cuts the file back to `size` bytes: what recovery does with a torn tail.
    void truncate(std::size_t size) {
        if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            throw std::runtime_error("cannot truncate journal '" + path_ +
                                     "': " + std::strerror(errno));
        }
    }

    [[nodiscard]] std::size_t size() const {
        struct stat st {};
        if (::fstat(fd_, &st) != 0) {
            throw std::runtime_error("cannot stat journal '" + path_ +
                                     "': " + std::strerror(errno));
        }
        return static_cast<std::size_t>(st.st_size);
    }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::uint64_t writes() const noexcept { return writes_; }
    [[nodiscard]] std::uint64_t syncs() const noexcept { return syncs_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

 private:
    int fd_;
    std::string path_;
    std::uint64_t writes_ = 0;
    std::uint64_t syncs_ = 0;
    std::uint64_t bytes_ = 0;
};

// What to do after each write of a batch of records.
enum class SyncPolicy : std::uint8_t {
    Never,       // leave it to the operating system: survives a crash of the program
    EveryFlush,  // fdatasync after every flush: survives the machine losing power
};

// The destination to give a JournalWriter: writes each flushed batch to the
// file and, if asked, syncs it.
class ToFile {
 public:
    explicit ToFile(JournalFile& file, SyncPolicy policy = SyncPolicy::Never) noexcept
        : file_(&file), policy_(policy) {}

    void operator()(std::span<const std::byte> bytes) const {
        file_->write(bytes);
        if (policy_ == SyncPolicy::EveryFlush) {
            file_->sync();
        }
    }

 private:
    JournalFile* file_;
    SyncPolicy policy_;
};

// Replaces the file at `path` with `bytes`, so that a crash at any moment
// leaves either the old file or the new one, whole, and never a mixture.
//
// It is how a snapshot is written. The steps, and what each is for:
//
//   write to "<path>.tmp"   the old snapshot is not touched while the new one
//                           is being written
//   fdatasync               the new one is on the device before it is given
//                           the real name
//   rename over `path`      within one filesystem this is atomic: at every
//                           instant the name refers to one whole file
//   fsync the directory     the rename itself is a change to the directory,
//                           and is not durable until the directory is. Some
//                           filesystems refuse this for a directory, so a
//                           failure here is ignored: the file is right
//                           either way, and what is at risk is only which of
//                           two good snapshots survives a power cut
//
// Throws std::runtime_error with the path and the system's reason on failure,
// leaving the old file as it was.
inline void write_file_atomically(const std::string& path, std::span<const std::byte> bytes) {
    const std::string tmp = path + ".tmp";
    const auto fail = [](const std::string& doing, const std::string& what, int err) {
        throw std::runtime_error("cannot " + doing + " '" + what + "': " + std::strerror(err));
    };
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fail("create", tmp, errno);
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            ::close(fd);
            ::unlink(tmp.c_str());
            fail("write", tmp, err);
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fdatasync(fd) != 0) {
        const int err = errno;
        ::close(fd);
        ::unlink(tmp.c_str());
        fail("sync", tmp, err);
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        fail("rename to", path, err);
    }
    const std::size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash + 1);
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
}

}  // namespace obe::journal
