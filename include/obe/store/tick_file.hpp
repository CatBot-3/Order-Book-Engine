#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "obe/feed/endian.hpp"
#include "obe/feed/messages.hpp"
#include "obe/store/tick_codec.hpp"
#include "obe/types.hpp"
#include "obe/util/crc32.hpp"

// The tick store: best-bid-and-offer updates on disk, in the order they
// happened, in blocks that can each be read on their own.
//
// WHY BLOCKS
//
// A compressing codec writes each tick as a difference from earlier ones, so
// reading tick number fifty million would mean decoding the forty-nine
// million before it. Cutting the stream into blocks and making the codec
// forget everything at the start of each (TickCodec::reset) bounds that: to
// read any tick, decode at most one block. The price is that the first ticks
// of every block have nothing to be a difference from. Bigger blocks compress
// a little better and seek a little worse; `tick_store record --block N` is
// there to measure it.
//
// THE FILE
//
//   file header   16 bytes   "OBET" | version u16 | codec id u16 |
//                            ticks per block u32 | zero u32
//   block         32 bytes   "BLK1" | crc u32 | ticks u32 | payload bytes u32 |
//                            earliest timestamp u64 | latest timestamp u64
//                 payload    the ticks, encoded by the codec from a reset
//   ... more blocks ...
//   index         24 bytes   "IDX1" | crc u32 | blocks u32 | symbols u32 |
//                            ticks u64
//                 32 each    per block: offset u64 | ticks u32 | payload u32 |
//                            earliest u64 | latest u64
//                 10 each    per symbol: locate u16 | symbol 8 chars
//   trailer       12 bytes   offset of the index u64 | "OBEX"
//
// All integers big-endian. Each crc is a CRC-32 of everything in its section
// after the crc field itself.
//
// The index is what makes a time-range query a seek: it says which blocks can
// hold ticks of that time without reading any of them. It is written last,
// because only then is it known.
//
// A FILE WITHOUT AN INDEX IS STILL A STORE
//
// A recorder that is killed never writes its index. Every block carries its
// own length and checksum, so the reader can walk the file from the start and
// rebuild the index from the blocks it finds whole, stopping at the first
// that is not. That is the same idea as the journal's torn tail
// (obe/journal/reader.hpp): what was completely written is kept, and what was
// being written is dropped. Such a file opens as Recovered. Only the symbol
// directory is lost, since it lives in the index.
//
// ONE STREAM, IN TIME ORDER
//
// All securities share the blocks. That makes "everything between 10:00 and
// 10:05" cheap and "one stock for the whole day" a scan of every block.
// Storing each security separately would reverse the two, and compress
// better, at the cost of thousands of open streams while recording. It is the
// first design decision to revisit if the queries turn out to be per symbol
// (docs/design.md).

namespace obe::store {

inline constexpr char kFileMagic[4] = {'O', 'B', 'E', 'T'};
inline constexpr char kBlockMagic[4] = {'B', 'L', 'K', '1'};
inline constexpr char kIndexMagic[4] = {'I', 'D', 'X', '1'};
inline constexpr char kTrailerMagic[4] = {'O', 'B', 'E', 'X'};
inline constexpr std::uint16_t kFileVersion = 1;
inline constexpr std::size_t kFileHeaderSize = 16;
inline constexpr std::size_t kBlockHeaderSize = 32;
inline constexpr std::size_t kIndexHeaderSize = 24;
inline constexpr std::size_t kIndexEntrySize = 32;
inline constexpr std::size_t kSymbolEntrySize = 10;
inline constexpr std::size_t kTrailerSize = 12;
inline constexpr std::uint32_t kDefaultTicksPerBlock = 4096;
inline constexpr std::uint32_t kMaxTicksPerBlock = 65'536;

// As the end of a range: no end. A range is half-open, [from, to), so no
// ordinary `to` could include a tick stamped with the largest time there is.
// This value is the exception: scan(from, kEndOfTime) leaves nothing out.
inline constexpr Nanos kEndOfTime = std::numeric_limits<Nanos>::max();

// What the index says about one block.
struct BlockInfo {
    std::uint64_t offset = 0;   // of the block's header, from the start of the file
    std::uint32_t count = 0;    // ticks in it
    std::uint32_t payload = 0;  // bytes of encoded ticks, after the header
    Nanos earliest = 0;         // the smallest timestamp in it
    Nanos latest = 0;           // the largest

    friend bool operator==(const BlockInfo&, const BlockInfo&) = default;
};

struct SymbolEntry {
    Locate locate = 0;
    feed::Symbol symbol{};

    friend bool operator==(const SymbolEntry&, const SymbolEntry&) = default;
};

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// Turns ticks into the bytes of a store and hands them to `out`, a callable
// taking a span: a file in the tools, a vector in the tests. Each block is
// handed over whole, in one call, when it is full.
//
// Ticks are stored in the order they are appended. Nothing requires their
// timestamps to rise: each block records its earliest and latest, and a query
// looks at every block whose range could matter.
template <TickCodec Codec, class Out>
class TickWriter {
 public:
    explicit TickWriter(Out out, std::uint32_t ticks_per_block = kDefaultTicksPerBlock)
        : out_(std::move(out)),
          per_block_(std::clamp<std::uint32_t>(ticks_per_block, 1, kMaxTicksPerBlock)),
          block_(kBlockHeaderSize + std::size_t{per_block_} * Codec::kMaxTickSize) {
        std::byte header[kFileHeaderSize];
        std::memcpy(header, kFileMagic, sizeof(kFileMagic));
        feed::store_be<std::uint16_t>(header + 4, kFileVersion);
        feed::store_be<std::uint16_t>(header + 6, Codec::kId);
        feed::store_be<std::uint32_t>(header + 8, per_block_);
        feed::store_be<std::uint32_t>(header + 12, 0);
        hand_over({header, kFileHeaderSize});
        codec_.reset();
    }

    // Adds one tick. Precondition: finish() has not been called.
    void append(const Tick& tick) {
        if (count_ == 0) {
            earliest_ = tick.timestamp;
            latest_ = tick.timestamp;
        } else {
            earliest_ = std::min(earliest_, tick.timestamp);
            latest_ = std::max(latest_, tick.timestamp);
        }
        used_ += codec_.encode(tick, block_.data() + kBlockHeaderSize + used_);
        ++count_;
        ++ticks_;
        if (count_ == per_block_) {
            flush_block();
        }
    }

    // Names a locate, for the directory in the index. Naming one again
    // replaces the earlier name.
    void add_symbol(Locate locate, const feed::Symbol& symbol) {
        for (SymbolEntry& entry : symbols_) {
            if (entry.locate == locate) {
                entry.symbol = symbol;
                return;
            }
        }
        symbols_.push_back({locate, symbol});
    }

    // Writes out the last, partly filled block, then the index and the
    // trailer. Calling it again does nothing. A store that is never finished
    // is still readable: see "a file without an index" above.
    void finish() {
        if (finished_) {
            return;
        }
        finished_ = true;
        flush_block();

        std::sort(symbols_.begin(), symbols_.end(),
                  [](const SymbolEntry& a, const SymbolEntry& b) { return a.locate < b.locate; });
        std::vector<std::byte> index(kIndexHeaderSize + index_.size() * kIndexEntrySize +
                                     symbols_.size() * kSymbolEntrySize + kTrailerSize);
        std::byte* p = index.data();
        std::memcpy(p, kIndexMagic, sizeof(kIndexMagic));
        feed::store_be<std::uint32_t>(p + 8, static_cast<std::uint32_t>(index_.size()));
        feed::store_be<std::uint32_t>(p + 12, static_cast<std::uint32_t>(symbols_.size()));
        feed::store_be<std::uint64_t>(p + 16, ticks_);
        p += kIndexHeaderSize;
        for (const BlockInfo& block : index_) {
            feed::store_be<std::uint64_t>(p, block.offset);
            feed::store_be<std::uint32_t>(p + 8, block.count);
            feed::store_be<std::uint32_t>(p + 12, block.payload);
            feed::store_be<std::uint64_t>(p + 16, block.earliest);
            feed::store_be<std::uint64_t>(p + 24, block.latest);
            p += kIndexEntrySize;
        }
        for (const SymbolEntry& entry : symbols_) {
            feed::store_be<std::uint16_t>(p, entry.locate);
            std::memcpy(p + 2, entry.symbol.raw.data(), 8);
            p += kSymbolEntrySize;
        }
        const std::size_t index_size = index.size() - kTrailerSize;
        feed::store_be<std::uint32_t>(index.data() + 4,
                                      util::crc32({index.data() + 8, index_size - 8}));
        feed::store_be<std::uint64_t>(p, bytes_);  // where the index starts
        std::memcpy(p + 8, kTrailerMagic, sizeof(kTrailerMagic));
        hand_over(index);
    }

    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }
    // Blocks handed over so far.
    [[nodiscard]] std::size_t blocks() const noexcept { return index_.size(); }
    // Bytes handed over so far, and how many of them are encoded ticks (the
    // rest is headers and the index).
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint64_t payload_bytes() const noexcept { return payload_bytes_; }
    [[nodiscard]] std::uint32_t ticks_per_block() const noexcept { return per_block_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    [[nodiscard]] Out& out() noexcept { return out_; }

 private:
    void hand_over(std::span<const std::byte> bytes) {
        out_(bytes);
        bytes_ += bytes.size();
    }

    void flush_block() {
        if (count_ == 0) {
            return;
        }
        std::byte* p = block_.data();
        std::memcpy(p, kBlockMagic, sizeof(kBlockMagic));
        feed::store_be<std::uint32_t>(p + 8, count_);
        feed::store_be<std::uint32_t>(p + 12, static_cast<std::uint32_t>(used_));
        feed::store_be<std::uint64_t>(p + 16, earliest_);
        feed::store_be<std::uint64_t>(p + 24, latest_);
        feed::store_be<std::uint32_t>(p + 4, util::crc32({p + 8, kBlockHeaderSize - 8 + used_}));
        index_.push_back({bytes_, count_, static_cast<std::uint32_t>(used_), earliest_, latest_});
        payload_bytes_ += used_;
        hand_over({p, kBlockHeaderSize + used_});
        count_ = 0;
        used_ = 0;
        codec_.reset();
    }

    Out out_;
    Codec codec_;
    std::uint32_t per_block_;
    std::vector<std::byte> block_;  // the block being filled: header, then payload
    std::size_t used_ = 0;          // payload bytes in it
    std::uint32_t count_ = 0;       // ticks in it
    Nanos earliest_ = 0;
    Nanos latest_ = 0;
    std::vector<BlockInfo> index_;
    std::vector<SymbolEntry> symbols_;
    std::uint64_t ticks_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t payload_bytes_ = 0;
    bool finished_ = false;
};

// A book listener that records every update it is given.
template <TickCodec Codec, class Out>
class TickRecorder {
 public:
    explicit TickRecorder(TickWriter<Codec, Out>& writer) noexcept : writer_(&writer) {}
    void on_bbo(const book::BboUpdate& update) { writer_->append(update); }

 private:
    TickWriter<Codec, Out>* writer_;
};

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

enum class OpenStatus : std::uint8_t {
    Complete,    // the index is there and sound
    Recovered,   // no usable index: the blocks were found by walking the file
    BadHeader,   // not a tick store, or a version this code does not read
    WrongCodec,  // a tick store written with another codec
};

[[nodiscard]] constexpr std::string_view to_string(OpenStatus s) noexcept {
    switch (s) {
        case OpenStatus::Complete:
            return "complete";
        case OpenStatus::Recovered:
            return "no index: recovered by walking the blocks";
        case OpenStatus::BadHeader:
            return "not a tick store this version can read";
        case OpenStatus::WrongCodec:
            return "written with another codec";
    }
    return "?";
}

// What the first sixteen bytes say, without needing to know the codec. It is
// how a tool finds out which codec to open a store with.
struct FileHeader {
    std::uint16_t version = 0;
    std::uint16_t codec = 0;
    std::uint32_t ticks_per_block = 0;
};

[[nodiscard]] inline std::optional<FileHeader> read_header(
    std::span<const std::byte> file) noexcept {
    if (file.size() < kFileHeaderSize ||
        std::memcmp(file.data(), kFileMagic, sizeof(kFileMagic)) != 0) {
        return std::nullopt;
    }
    FileHeader h;
    h.version = feed::load_be<std::uint16_t>(file.data() + 4);
    h.codec = feed::load_be<std::uint16_t>(file.data() + 6);
    h.ticks_per_block = feed::load_be<std::uint32_t>(file.data() + 8);
    if (h.version != kFileVersion || h.ticks_per_block == 0 ||
        h.ticks_per_block > kMaxTicksPerBlock ||
        feed::load_be<std::uint32_t>(file.data() + 12) != 0) {
        return std::nullopt;
    }
    return h;
}

// How a scan went.
struct ScanResult {
    static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

    bool ok = true;                    // false: a block was damaged; see bad_block
    std::uint64_t ticks = 0;           // ticks handed to the visitor
    std::uint64_t blocks_read = 0;     // blocks decoded
    std::uint64_t blocks_skipped = 0;  // blocks the index ruled out unread
    std::size_t bad_block = kNone;     // the block that could not be read

    friend bool operator==(const ScanResult&, const ScanResult&) = default;
};

// Reads a store held in memory (a mapped file). Nothing here reads outside
// the span it was given, whatever the bytes are.
template <TickCodec Codec>
class TickReader {
 public:
    explicit TickReader(std::span<const std::byte> file) : file_(file) {
        const std::optional<FileHeader> header = read_header(file_);
        if (!header) {
            status_ = OpenStatus::BadHeader;
            return;
        }
        if (header->codec != Codec::kId) {
            status_ = OpenStatus::WrongCodec;
            return;
        }
        per_block_ = header->ticks_per_block;
        if (load_index()) {
            status_ = OpenStatus::Complete;
            good_bytes_ = file_.size();
        } else {
            status_ = OpenStatus::Recovered;
            walk_blocks();
        }
        for (const BlockInfo& block : blocks_) {
            ticks_ += block.count;
        }
    }

    [[nodiscard]] OpenStatus status() const noexcept { return status_; }
    // The store can be read: Complete or Recovered.
    [[nodiscard]] bool usable() const noexcept {
        return status_ == OpenStatus::Complete || status_ == OpenStatus::Recovered;
    }
    [[nodiscard]] const std::vector<BlockInfo>& blocks() const noexcept { return blocks_; }
    [[nodiscard]] const std::vector<SymbolEntry>& symbols() const noexcept { return symbols_; }
    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }
    [[nodiscard]] std::uint32_t ticks_per_block() const noexcept { return per_block_; }
    // Bytes of the file that are accounted for. For a Recovered store this is
    // the end of the last whole block: what follows was being written when
    // the recorder stopped.
    [[nodiscard]] std::size_t good_bytes() const noexcept { return good_bytes_; }

    [[nodiscard]] std::optional<Locate> locate_of(std::string_view symbol) const noexcept {
        for (const SymbolEntry& entry : symbols_) {
            if (entry.symbol.view() == symbol) {
                return entry.locate;
            }
        }
        return std::nullopt;
    }

    // Calls f(const Tick&) for every tick with from <= timestamp < to, in the
    // order they were stored; with to == kEndOfTime, for every tick from
    // `from` on. f returns bool: false stops the scan (which is still ok).
    // Blocks whose range cannot contain such a tick are not read.
    //
    // A block is checked against its checksum before any of its ticks is
    // handed over, so the visitor never sees a tick from a block that was
    // damaged after it was written. On damage the scan stops there: ok is
    // false and bad_block says which.
    template <class F>
    ScanResult scan(Nanos from, Nanos to, F&& f) const {
        ScanResult result;
        Codec codec;
        const bool no_end = to == kEndOfTime;
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            const BlockInfo& block = blocks_[i];
            if ((!no_end && block.earliest >= to) || block.latest < from) {
                ++result.blocks_skipped;
                continue;
            }
            if (!intact(block)) {
                result.ok = false;
                result.bad_block = i;
                return result;
            }
            ++result.blocks_read;
            const std::byte* p = file_.data() + block.offset + kBlockHeaderSize;
            const std::byte* const end = p + block.payload;
            codec.reset();
            for (std::uint32_t k = 0; k < block.count; ++k) {
                Tick tick;
                const std::size_t n = codec.decode(p, end, tick);
                if (n == 0 || n > static_cast<std::size_t>(end - p)) {
                    // The checksum matched and a tick does not decode: the
                    // block was written wrongly, not damaged afterwards. The
                    // ticks before this one have already been handed over.
                    result.ok = false;
                    result.bad_block = i;
                    return result;
                }
                p += n;
                if (tick.timestamp >= from && (no_end || tick.timestamp < to)) {
                    ++result.ticks;
                    if (!f(std::as_const(tick))) {
                        return result;
                    }
                }
            }
            if (p != end) {
                result.ok = false;  // bytes left over after the last tick
                result.bad_block = i;
                return result;
            }
        }
        return result;
    }

    // Every tick in the store.
    template <class F>
    ScanResult scan(F&& f) const {
        return scan(0, kEndOfTime, std::forward<F>(f));
    }

 private:
    // Whether the block's bytes match its checksum.
    [[nodiscard]] bool intact(const BlockInfo& block) const noexcept {
        const std::byte* p = file_.data() + block.offset;
        return util::crc32({p + 8, kBlockHeaderSize - 8 + block.payload}) ==
               feed::load_be<std::uint32_t>(p + 4);
    }

    // Reads the header of a block at `at`, if one fits there and its fields
    // make sense. Does not check the checksum.
    [[nodiscard]] std::optional<BlockInfo> block_at(std::size_t at,
                                                    std::size_t limit) const noexcept {
        if (limit < kBlockHeaderSize || at > limit - kBlockHeaderSize) {
            return std::nullopt;
        }
        const std::byte* p = file_.data() + at;
        if (std::memcmp(p, kBlockMagic, sizeof(kBlockMagic)) != 0) {
            return std::nullopt;
        }
        BlockInfo block;
        block.offset = at;
        block.count = feed::load_be<std::uint32_t>(p + 8);
        block.payload = feed::load_be<std::uint32_t>(p + 12);
        block.earliest = feed::load_be<std::uint64_t>(p + 16);
        block.latest = feed::load_be<std::uint64_t>(p + 24);
        if (block.count == 0 || block.count > per_block_ || block.earliest > block.latest ||
            block.payload > std::size_t{block.count} * Codec::kMaxTickSize ||
            block.payload > limit - kBlockHeaderSize - at) {
            return std::nullopt;
        }
        return block;
    }

    // Tries to use the index at the end of the file. It is believed only if
    // it is whole, its checksum matches, and it describes exactly the blocks
    // that lie between the file header and itself.
    [[nodiscard]] bool load_index() {
        const std::size_t size = file_.size();
        if (size < kFileHeaderSize + kIndexHeaderSize + kTrailerSize ||
            std::memcmp(file_.data() + size - 4, kTrailerMagic, sizeof(kTrailerMagic)) != 0) {
            return false;
        }
        const std::size_t index_end = size - kTrailerSize;
        const std::uint64_t at = feed::load_be<std::uint64_t>(file_.data() + index_end);
        if (at < kFileHeaderSize || at > index_end - kIndexHeaderSize) {
            return false;
        }
        const std::byte* p = file_.data() + at;
        if (std::memcmp(p, kIndexMagic, sizeof(kIndexMagic)) != 0) {
            return false;
        }
        const std::size_t blocks = feed::load_be<std::uint32_t>(p + 8);
        const std::size_t symbols = feed::load_be<std::uint32_t>(p + 12);
        const std::uint64_t ticks = feed::load_be<std::uint64_t>(p + 16);
        // The counts are held to the length before anything is sized by them.
        const std::size_t room = index_end - at - kIndexHeaderSize;
        if (blocks > room / kIndexEntrySize || symbols > room / kSymbolEntrySize ||
            blocks * kIndexEntrySize + symbols * kSymbolEntrySize != room) {
            return false;
        }
        if (util::crc32({p + 8, index_end - at - 8}) != feed::load_be<std::uint32_t>(p + 4)) {
            return false;
        }

        std::vector<BlockInfo> found;
        found.reserve(blocks);
        std::size_t expected = kFileHeaderSize;
        std::uint64_t total = 0;
        p += kIndexHeaderSize;
        for (std::size_t i = 0; i < blocks; ++i) {
            BlockInfo listed;
            listed.offset = feed::load_be<std::uint64_t>(p);
            listed.count = feed::load_be<std::uint32_t>(p + 8);
            listed.payload = feed::load_be<std::uint32_t>(p + 12);
            listed.earliest = feed::load_be<std::uint64_t>(p + 16);
            listed.latest = feed::load_be<std::uint64_t>(p + 24);
            p += kIndexEntrySize;
            // Each block must start where the one before it ended, and the
            // block's own header must say what the index says.
            const std::optional<BlockInfo> actual = block_at(expected, at);
            if (listed.offset != expected || !actual || *actual != listed) {
                return false;
            }
            expected += kBlockHeaderSize + listed.payload;
            total += listed.count;
            found.push_back(listed);
        }
        if (expected != at || total != ticks) {
            return false;
        }
        std::vector<SymbolEntry> names(symbols);
        for (SymbolEntry& entry : names) {
            entry.locate = feed::load_be<std::uint16_t>(p);
            std::memcpy(entry.symbol.raw.data(), p + 2, 8);
            p += kSymbolEntrySize;
        }
        blocks_ = std::move(found);
        symbols_ = std::move(names);
        return true;
    }

    // Finds the blocks by walking the file from its header, keeping each one
    // that is whole and matches its checksum, and stopping at the first that
    // does not.
    void walk_blocks() {
        std::size_t at = kFileHeaderSize;
        for (;;) {
            const std::optional<BlockInfo> block = block_at(at, file_.size());
            if (!block || !intact(*block)) {
                break;
            }
            blocks_.push_back(*block);
            at += kBlockHeaderSize + block->payload;
        }
        good_bytes_ = at;
    }

    std::span<const std::byte> file_;
    OpenStatus status_ = OpenStatus::BadHeader;
    std::uint32_t per_block_ = 0;
    std::vector<BlockInfo> blocks_;
    std::vector<SymbolEntry> symbols_;
    std::uint64_t ticks_ = 0;
    std::size_t good_bytes_ = 0;
};

}  // namespace obe::store
