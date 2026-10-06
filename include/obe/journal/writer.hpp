#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "obe/feed/codec.hpp"
#include "obe/feed/endian.hpp"
#include "obe/journal/records.hpp"
#include "obe/util/crc32.hpp"

// Turns records into the bytes of a journal.
//
// The writer only builds bytes and numbers the records. Where the bytes go is
// the business of `out`, a callable taking a span: a file (file.hpp), a vector
// in the tests. That separation is what lets the tests cut a journal off at
// every byte without touching a disk.
//
// append() adds to a buffer and flush() hands the buffer over in one piece.
// How often to flush, and whether to force the result onto the disk
// afterwards, is the caller's trade between speed and how much can be lost:
// see "group commit" in docs/design.md.

namespace obe::journal {

// The 16 bytes at the start of a journal file.
inline void encode_file_header(std::byte* out, std::uint64_t first_seq) noexcept {
    std::memcpy(out, kMagic, sizeof(kMagic));
    feed::store_be<std::uint16_t>(out + 4, kVersion);
    feed::store_be<std::uint16_t>(out + 6, 0);
    feed::store_be<std::uint64_t>(out + 8, first_seq);
}

template <class Out>
class JournalWriter {
 public:
    // `first_seq` is the number the first record appended will get.
    //
    // A new journal starts with a file header; pass write_header = false when
    // carrying on at the end of an existing one, after a recovery.
    explicit JournalWriter(Out out, std::uint64_t first_seq = 1, bool write_header = true)
        : out_(std::move(out)), next_seq_(first_seq) {
        if (write_header) {
            buffer_.resize(kFileHeaderSize);
            encode_file_header(buffer_.data(), first_seq);
        }
    }

    // Adds a record to the buffer and returns its sequence number. Nothing
    // reaches `out` until flush().
    template <class Record>
    std::uint64_t append(const Record& record) {
        const std::size_t body = feed::wire_size(record);
        const std::size_t at = buffer_.size();
        buffer_.resize(at + kRecordHeaderSize + body);
        std::byte* p = buffer_.data() + at;
        feed::store_be<std::uint16_t>(p + 4, static_cast<std::uint16_t>(body));
        feed::store_be<std::uint64_t>(p + 6, next_seq_);
        feed::encode(record, p + kRecordHeaderSize);
        const std::uint32_t crc = util::crc32({p + 4, kRecordHeaderSize - 4 + body});
        feed::store_be<std::uint32_t>(p, crc);
        ++records_;
        return next_seq_++;
    }

    // Hands everything buffered to `out`, in one call.
    void flush() {
        if (buffer_.empty()) {
            return;
        }
        out_(std::span<const std::byte>(buffer_));
        bytes_ += buffer_.size();
        buffer_.clear();
    }

    // The number the next record will get.
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_seq_; }
    [[nodiscard]] std::uint64_t records() const noexcept { return records_; }
    // Bytes handed to `out` so far, and bytes still waiting for a flush.
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t pending() const noexcept { return buffer_.size(); }

    [[nodiscard]] Out& out() noexcept { return out_; }

 private:
    Out out_;
    std::vector<std::byte> buffer_;
    std::uint64_t next_seq_;
    std::uint64_t records_ = 0;
    std::uint64_t bytes_ = 0;
};

}  // namespace obe::journal
