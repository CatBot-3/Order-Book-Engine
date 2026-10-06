#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "obe/feed/endian.hpp"
#include "obe/journal/records.hpp"
#include "obe/util/crc32.hpp"

// Reads a journal back, one record at a time, and says how it ended.
//
// A journal that was being written when the machine stopped does not end
// neatly. Its last record may be half there, or all there with part of it
// never having reached the disk. That is the normal case, not a fault, and the
// reader's main job is to tell it apart from the two cases that are faults:
//
//   TornTail   The journal stops being readable at some point and NOTHING
//              readable follows. Everything before that point is good. This
//              is what a crash leaves. Cut the file there and carry on.
//   Corrupt    The journal stops being readable at some point and good
//              records FOLLOW it. Something damaged the middle of the file.
//              The records after the damage cannot be applied, because the
//              ones that are missing came first; cutting the file here would
//              throw good history away, so it is reported and left alone.
//
// The difference cannot be seen by looking at the bad record alone: a damaged
// length field looks exactly like a record that runs off the end of the file.
// So after a failure the reader looks ahead for anything that parses as a
// later record, and decides by whether it finds one.
//
// Nothing here reads outside the span it was given, whatever the bytes are.

namespace obe::journal {

enum class ReadStatus : std::uint8_t {
    Ok,             // a record was read
    End,            // the journal ended cleanly, after a whole record
    TornTail,       // unreadable from here to the end: what a crash leaves
    Corrupt,        // unreadable here, with good records after it
    BadHeader,      // not a journal, or a version this code does not read
    BadSequence,    // an intact record whose number is not the next one
    UnknownRecord,  // an intact record of a type or size this code does not know
};

[[nodiscard]] constexpr std::string_view to_string(ReadStatus s) noexcept {
    switch (s) {
        case ReadStatus::Ok:
            return "ok";
        case ReadStatus::End:
            return "clean end";
        case ReadStatus::TornTail:
            return "torn tail";
        case ReadStatus::Corrupt:
            return "corrupt record with good records after it";
        case ReadStatus::BadHeader:
            return "not a journal this version can read";
        case ReadStatus::BadSequence:
            return "sequence number out of order";
        case ReadStatus::UnknownRecord:
            return "record of an unknown type or size";
    }
    return "?";
}

// One record, still in the journal's bytes.
struct RecordView {
    std::uint64_t seq = 0;
    char type = 0;
    const std::byte* body = nullptr;  // the type byte, then the fields
    std::size_t size = 0;             // of the body
};

class JournalReader {
 public:
    explicit JournalReader(std::span<const std::byte> journal) noexcept : buf_(journal) {
        if (buf_.size() < kFileHeaderSize) {
            // A file that was created and never got its header, or only the
            // start of one, holds no records. If what is there could be the
            // start of a header, that is a crash at the very beginning.
            const std::size_t n = buf_.size() < sizeof(kMagic) ? buf_.size() : sizeof(kMagic);
            const bool prefix = n == 0 || std::memcmp(buf_.data(), kMagic, n) == 0;
            done_ = prefix ? ReadStatus::TornTail : ReadStatus::BadHeader;
            return;
        }
        if (std::memcmp(buf_.data(), kMagic, sizeof(kMagic)) != 0 ||
            feed::load_be<std::uint16_t>(buf_.data() + 4) != kVersion ||
            feed::load_be<std::uint16_t>(buf_.data() + 6) != 0) {
            done_ = ReadStatus::BadHeader;
            return;
        }
        first_seq_ = feed::load_be<std::uint64_t>(buf_.data() + 8);
        next_seq_ = first_seq_;
        pos_ = kFileHeaderSize;
    }

    // Reads the next record into `out`. Any result but Ok is final: calling
    // again returns the same thing.
    [[nodiscard]] ReadStatus next(RecordView& out) noexcept {
        if (done_ != ReadStatus::Ok) {
            return done_;
        }
        if (pos_ == buf_.size()) {
            return ReadStatus::End;
        }
        RecordView view;
        std::size_t length = 0;
        if (!intact(pos_, view, length)) {
            return done_ =
                       later_record_exists(pos_ + 1) ? ReadStatus::Corrupt : ReadStatus::TornTail;
        }
        // From here the record is exactly what was written. If it is still
        // not usable, that is a mistake in what was written, not damage.
        if (view.seq != next_seq_) {
            return done_ = ReadStatus::BadSequence;
        }
        if (view.size == 0 || view.size != body_size(view.type)) {
            return done_ = ReadStatus::UnknownRecord;
        }
        out = view;
        pos_ += length;
        ++next_seq_;
        return ReadStatus::Ok;
    }

    // Bytes of the journal that are good: the header and every record read so
    // far. After a TornTail this is where to cut the file.
    [[nodiscard]] std::size_t offset() const noexcept { return pos_; }

    // The number of the first record in the file, and of the record that
    // would come after the last one read.
    [[nodiscard]] std::uint64_t first_sequence() const noexcept { return first_seq_; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_seq_; }

 private:
    // Is there a whole record at `at` whose checksum matches? Fills in the
    // view and the record's total length if so.
    [[nodiscard]] bool intact(std::size_t at, RecordView& view,
                              std::size_t& length) const noexcept {
        const std::size_t left = buf_.size() - at;
        if (left < kRecordHeaderSize) {
            return false;
        }
        const std::byte* p = buf_.data() + at;
        const std::size_t body = feed::load_be<std::uint16_t>(p + 4);
        if (body > left - kRecordHeaderSize) {
            return false;
        }
        const std::uint32_t stored = feed::load_be<std::uint32_t>(p);
        if (util::crc32({p + 4, kRecordHeaderSize - 4 + body}) != stored) {
            return false;
        }
        view.seq = feed::load_be<std::uint64_t>(p + 6);
        view.type = body == 0 ? char{0} : static_cast<char>(p[kRecordHeaderSize]);
        view.body = p + kRecordHeaderSize;
        view.size = body;
        length = kRecordHeaderSize + body;
        return true;
    }

    // Does anything from `from` onwards parse as a record later than the ones
    // already read? Only run after a failure, so its cost is not on any path
    // that matters; it is still kept linear by checking the cheap things (a
    // known type with the size that type has) before computing a checksum.
    //
    // "Later" is part of the test. An intact record with a number already
    // passed is stale: after a crash a filesystem can leave old blocks showing
    // at the end of a file that was being extended, and they may hold records
    // of an earlier journal. Such a record is nothing this journal has not
    // already given, so finding one does not make the failure corruption.
    [[nodiscard]] bool later_record_exists(std::size_t from) const noexcept {
        for (std::size_t at = from; at + kRecordHeaderSize < buf_.size(); ++at) {
            const std::byte* p = buf_.data() + at;
            const std::size_t body = feed::load_be<std::uint16_t>(p + 4);
            if (body == 0 || body > kMaxBodySize ||
                body != body_size(static_cast<char>(p[kRecordHeaderSize]))) {
                continue;
            }
            RecordView view;
            std::size_t length = 0;
            if (intact(at, view, length) && view.seq >= next_seq_) {
                return true;
            }
        }
        return false;
    }

    std::span<const std::byte> buf_;
    std::size_t pos_ = 0;
    std::uint64_t first_seq_ = 0;
    std::uint64_t next_seq_ = 0;
    ReadStatus done_ = ReadStatus::Ok;  // Ok while reading; then the final status
};

}  // namespace obe::journal
