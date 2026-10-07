// libFuzzer target for the tick store: its reader, its codecs and the varint
// they are built from.
//
// The first byte of the input chooses what the rest is taken as.
//
// Even: the rest is given to the readers as it is, as a damaged or hostile
// file would be. Checked on arbitrary bytes:
//   1. Nothing reads outside its input (ASan) or executes undefined behaviour
//      (UBSan).
//   2. A store that opens describes itself consistently: its blocks lie in
//      the file, one after another, and add up to the ticks it reports.
//   3. A scan hands over no more ticks than the store reports, says so when
//      it hands over fewer, and a scan of a range hands over only ticks in
//      the range.
//   4. A varint that decodes encodes back to the same bytes.
//
// Odd: the rest is turned into ticks, and a store is built from them with the
// real writer. Random bytes almost never carry a correct checksum, so without
// this the fuzzer would never get past the first block header. Checked:
//   5. The store opens complete and gives back exactly the ticks, whole and
//      for a range.
//   6. Cut anywhere, it gives back the ticks of its whole blocks.
//   7. With any one byte changed it never hands over a tick that was not
//      written, and when ticks are missing either the way it opened or the
//      way the scan ended says so.
//
// Both codecs are run. Until DeltaCodec is written its functions throw, and
// the target leaves it out; from then on this fuzzer is its harshest judge.
//
//   cmake --preset fuzz && cmake --build --preset fuzz
//   build/fuzz/fuzz/tick_store_fuzz -max_len=2048 -max_total_time=60 fuzz/tick_seeds

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "obe/store/codecs.hpp"
#include "obe/store/tick_codec.hpp"
#include "obe/store/tick_file.hpp"
#include "obe/types.hpp"
#include "obe/util/todo.hpp"
#include "obe/util/varint.hpp"

namespace {

using namespace obe;
using store::kEndOfTime;
using store::OpenStatus;
using store::ScanResult;
using store::Tick;

void require(bool ok) {
    if (!ok) {
        std::abort();
    }
}

struct AppendTo {
    std::vector<std::byte>* bytes;
    void operator()(std::span<const std::byte> more) const {
        bytes->insert(bytes->end(), more.begin(), more.end());
    }
};

// Hands out the input a field at a time; zeros when it runs out.
class Input {
 public:
    explicit Input(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool empty() const { return at_ >= bytes_.size(); }
    [[nodiscard]] std::uint64_t take(std::size_t n) {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            v = (v << 8) | (at_ < bytes_.size() ? static_cast<std::uint64_t>(bytes_[at_]) : 0);
            ++at_;
        }
        return v;
    }

 private:
    std::span<const std::byte> bytes_;
    std::size_t at_ = 0;
};

bool in_range(const Tick& tick, Nanos from, Nanos to) {
    return tick.timestamp >= from && (to == kEndOfTime || tick.timestamp < to);
}

template <class Codec>
std::vector<Tick> scan_all(const store::TickReader<Codec>& reader, ScanResult& result) {
    std::vector<Tick> out;
    result = reader.scan([&out](const Tick& tick) {
        out.push_back(tick);
        return true;
    });
    return out;
}

// --- Arbitrary bytes -----------------------------------------------------------------

template <class Codec>
void raw_store(std::span<const std::byte> bytes) {
    const store::TickReader<Codec> reader(bytes);
    if (!reader.usable()) {
        require(reader.ticks() == 0 && reader.blocks().empty());
        return;
    }
    std::size_t at = store::kFileHeaderSize;
    std::uint64_t total = 0;
    for (const store::BlockInfo& block : reader.blocks()) {
        require(block.offset == at);
        require(block.count >= 1 && block.count <= reader.ticks_per_block());
        require(block.earliest <= block.latest);
        at += store::kBlockHeaderSize + block.payload;
        require(at <= bytes.size());
        total += block.count;
    }
    require(total == reader.ticks());
    require(reader.good_bytes() <= bytes.size());
    if (reader.status() == OpenStatus::Recovered) {
        require(reader.good_bytes() == at);
        require(reader.symbols().empty());
    }

    ScanResult whole;
    const std::vector<Tick> ticks = scan_all(reader, whole);
    require(whole.ticks == ticks.size());
    require(ticks.size() <= reader.ticks());
    if (whole.ok) {
        require(ticks.size() == reader.ticks());
        require(whole.bad_block == ScanResult::kNone);
    } else {
        require(whole.bad_block < reader.blocks().size());
    }

    // A range, chosen by the bytes themselves.
    Input input(bytes.subspan(std::min<std::size_t>(bytes.size(), store::kFileHeaderSize)));
    const Nanos from = input.take(8);
    const Nanos to = input.take(8);
    std::uint64_t inside = 0;
    for (const Tick& tick : ticks) {
        inside += in_range(tick, from, to) ? 1U : 0U;
    }
    std::uint64_t handed = 0;
    const ScanResult ranged = reader.scan(from, to, [&](const Tick& tick) {
        require(in_range(tick, from, to));
        ++handed;
        return true;
    });
    require(ranged.ticks == handed);
    if (whole.ok) {
        // Never more than are there. Fewer is possible only if a block's
        // header misstates its own time range, which a checksum cannot rule
        // out in a file made to deceive.
        require(handed <= inside);
    }
}

void raw_varint(std::span<const std::byte> bytes) {
    std::uint64_t value = 0;
    const std::size_t n = util::get_varint(bytes.data(), bytes.data() + bytes.size(), value);
    require(n <= bytes.size() && n <= util::kMaxVarintSize);
    if (n != 0) {
        std::byte again[util::kMaxVarintSize];
        require(util::put_varint(value, again) == n);
        require(util::varint_size(value) == n);
        require(std::memcmp(again, bytes.data(), n) == 0);
    }
    require(util::unzigzag(util::zigzag(static_cast<std::int64_t>(value))) ==
            static_cast<std::int64_t>(value));
}

// --- Built from the input --------------------------------------------------------------

// Ticks that are mostly close to the one before, as real ones are, so that a
// compressing codec's short encodings are reached as well as its long ones.
std::vector<Tick> ticks_from(Input& in) {
    std::vector<Tick> ticks;
    Tick tick{1, 34'200'000'000'000ULL, {1'000'000, 100, 1'000'100, 100}};
    while (!in.empty() && ticks.size() < 200) {
        const std::uint64_t how = in.take(1);
        switch (how % 8) {
            case 0:
                tick.timestamp += in.take(1);
                break;
            case 1:
                tick.locate = static_cast<Locate>(in.take(1) % 4);
                tick.bbo.bid_qty = in.take(1) * 100;
                break;
            case 2:
                tick.bbo.bid_price += 100;
                tick.bbo.ask_price += 100;
                break;
            case 3:
                tick.bbo.ask_qty -= in.take(1);
                break;
            case 4:
                tick.bbo.ask_price -= static_cast<Price>(in.take(2));
                break;
            case 5:
                tick.timestamp -= in.take(2);  // time going back
                tick.locate = static_cast<Locate>(in.take(2));
                break;
            case 6:
                break;  // the same tick again
            default:    // anything at all
                tick.timestamp = in.take(8);
                tick.locate = static_cast<Locate>(in.take(2));
                tick.bbo.bid_price = static_cast<Price>(in.take(4));
                tick.bbo.bid_qty = in.take(8);
                tick.bbo.ask_price = static_cast<Price>(in.take(4));
                tick.bbo.ask_qty = in.take(8);
                break;
        }
        ticks.push_back(tick);
    }
    return ticks;
}

bool starts(const std::vector<Tick>& all, const std::vector<Tick>& got) {
    return got.size() <= all.size() && std::equal(got.begin(), got.end(), all.begin());
}

template <class Codec>
void built_store(std::span<const std::byte> bytes) {
    Input in(bytes);
    const auto per_block = static_cast<std::uint32_t>(1 + in.take(1) % 16);
    const Nanos from = in.take(8);
    const Nanos to = in.take(8);
    const std::size_t where = in.take(2);
    const auto change = static_cast<std::byte>(1 + in.take(1) % 255);
    const std::vector<Tick> ticks = ticks_from(in);

    std::vector<std::byte> file;
    store::TickWriter<Codec, AppendTo> writer(AppendTo{&file}, per_block);
    for (const Tick& tick : ticks) {
        writer.append(tick);
    }
    writer.finish();
    file.shrink_to_fit();  // so that the end of the file is the end of an allocation

    // 5. What was written comes back.
    const store::TickReader<Codec> reader(file);
    require(reader.status() == OpenStatus::Complete);
    require(reader.ticks() == ticks.size());
    ScanResult whole;
    require(scan_all(reader, whole) == ticks);
    require(whole.ok);
    std::vector<Tick> expected;
    for (const Tick& tick : ticks) {
        if (in_range(tick, from, to)) {
            expected.push_back(tick);
        }
    }
    std::vector<Tick> ranged;
    const ScanResult range_result = reader.scan(from, to, [&ranged](const Tick& tick) {
        ranged.push_back(tick);
        return true;
    });
    require(range_result.ok && ranged == expected);
    require(range_result.blocks_read + range_result.blocks_skipped == reader.blocks().size());

    // 6. Cut anywhere.
    const std::size_t cut = where % file.size();
    {
        const std::vector<std::byte> part(file.begin(),
                                          file.begin() + static_cast<std::ptrdiff_t>(cut));
        const store::TickReader<Codec> cut_reader(part);
        if (cut < store::kFileHeaderSize) {
            require(!cut_reader.usable());
        } else {
            require(cut_reader.status() == OpenStatus::Recovered);
            std::uint64_t whole_ticks = 0;
            for (const store::BlockInfo& block : reader.blocks()) {
                if (block.offset + store::kBlockHeaderSize + block.payload <= cut) {
                    whole_ticks += block.count;
                }
            }
            ScanResult result;
            const std::vector<Tick> got = scan_all(cut_reader, result);
            require(result.ok && got.size() == whole_ticks && starts(ticks, got));
        }
    }

    // 7. One byte changed.
    {
        std::vector<std::byte> damaged = file;
        damaged[cut] ^= change;
        const store::TickReader<Codec> damaged_reader(damaged);
        ScanResult result;
        const std::vector<Tick> got = scan_all(damaged_reader, result);
        require(starts(ticks, got));
        if (got.size() < ticks.size()) {
            require(damaged_reader.status() != OpenStatus::Complete || !result.ok);
        }
    }
}

// Runs f, unless it turns out to need the hand-written codec and that is not
// there yet. Returns whether it ran to the end.
template <class F>
bool unless_unwritten(F&& f) {
    try {
        f();
    } catch (const util::Unimplemented&) {
        return false;
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    // A copy of exactly the right size: a read past the input is then a read
    // past an allocation.
    std::vector<std::byte> bytes(size - 1);
    if (size > 1) {
        std::memcpy(bytes.data(), data + 1, size - 1);
    }
    if (data[0] % 2 == 0) {
        raw_store<store::RawCodec>(bytes);
        static_cast<void>(unless_unwritten([&bytes] { raw_store<store::DeltaCodec>(bytes); }));
        raw_varint(bytes);
    } else {
        built_store<store::RawCodec>(bytes);
        static_cast<void>(unless_unwritten([&bytes] { built_store<store::DeltaCodec>(bytes); }));
    }
    return 0;
}
