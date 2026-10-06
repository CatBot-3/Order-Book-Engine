// libFuzzer target for the journal and the snapshot.
//
// The first byte of the input chooses what the rest is taken as.
//
// Even: the rest is fed to the readers as it is, as a damaged or hostile file
// would be. Checked on arbitrary bytes:
//   1. Nothing reads outside its input (ASan) or executes undefined behaviour
//      (UBSan).
//   2. The part of a journal the reader vouches for is itself a journal that
//      ends cleanly and holds the same records. "Cut the file here and carry
//      on" would be bad advice otherwise.
//   3. replay() gives an engine exactly the records the reader read.
//   4. Bytes that decode as a snapshot encode back to exactly those bytes.
//
// Odd: the rest is turned into requests, and a journal is built from them
// with the real writer. Random bytes almost never carry a correct checksum,
// so without this the fuzzer would never get past the first record. Checked:
//   5. The journal replays cleanly, and the engine is given the same calls,
//      in the same order, as when the requests are made directly.
//   6. Cut the journal anywhere and the engine is given the calls of the
//      whole records before the cut, and the result says the journal can be
//      carried on.
//   7. Change any one byte and the replay is NOT reported clean, and what the
//      engine was given is still a prefix of the requests: damage is never
//      applied and never goes unnoticed.
//   8. A snapshot built from the same bytes survives encode and decode, and
//      is refused with any one byte changed or with its end missing.
//
// The engine here only keeps a running hash of the calls it receives. A real
// engine holds a book for every possible locate and takes milliseconds to
// create, which would slow the fuzzer a thousandfold; that a real engine
// given the same calls ends in the same state is its own determinism, and
// the unit tests cover it.
//
//   cmake --preset fuzz && cmake --build --preset fuzz
//   build/fuzz/fuzz/journal_fuzz -max_len=2048 -max_total_time=60 fuzz/journal_seeds

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/output_hash.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/journal/reader.hpp"
#include "obe/journal/records.hpp"
#include "obe/journal/replay.hpp"
#include "obe/journal/snapshot.hpp"
#include "obe/journal/writer.hpp"
#include "obe/types.hpp"

namespace {

using namespace obe;
using engine::detail::mix;
using journal::ReadStatus;

void require(bool ok) {
    if (!ok) {
        std::abort();
    }
}

// An engine that remembers what it was asked, as a hash after each call.
struct CallLog {
    std::uint64_t hash = engine::detail::kHashSeed;
    std::vector<std::uint64_t> after;  // the hash after each call

    void note() { after.push_back(hash); }

    bool add_instrument(Locate locate, const feed::Symbol& symbol, Nanos now) {
        hash = mix(mix(mix(hash, 'I'), locate), now);
        for (const char c : symbol.raw) {
            hash = mix(hash, static_cast<unsigned char>(c));
        }
        note();
        return true;
    }
    OrderId submit(const engine::NewOrder& o, Nanos now) {
        hash = mix(mix(mix(mix(hash, 'S'), now), o.owner), o.token);
        hash = mix(mix(mix(hash, o.locate), static_cast<unsigned char>(o.side)), o.qty);
        hash = mix(mix(mix(hash, o.price), static_cast<unsigned char>(o.kind)),
                   static_cast<unsigned char>(o.tif));
        note();
        return 1;
    }
    bool cancel(engine::OwnerId owner, OrderId id, Nanos now) {
        hash = mix(mix(mix(mix(hash, 'X'), now), owner), id);
        note();
        return true;
    }
    OrderId replace(engine::OwnerId owner, OrderId id, Qty qty, Price price, Nanos now) {
        hash = mix(mix(mix(mix(mix(mix(hash, 'U'), now), owner), id), qty), price);
        note();
        return id;
    }

    // The rest of the engine contract, which replay does not use.
    [[nodiscard]] std::optional<book::Level> best(Locate, Side) const { return std::nullopt; }
    template <class F>
    void for_each_level(Locate, Side, F&&) const {}
    template <class F>
    void for_each_order(Locate, Side, F&&) const {}
    [[nodiscard]] std::size_t open_orders() const { return 0; }
    [[nodiscard]] bool listed(Locate) const { return false; }
    [[nodiscard]] engine::EngineStats stats() const { return {}; }
};
static_assert(engine::EngineLike<CallLog>);

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
    [[nodiscard]] std::uint8_t byte() { return static_cast<std::uint8_t>(take(1)); }
    [[nodiscard]] std::uint16_t u16() { return static_cast<std::uint16_t>(take(2)); }
    [[nodiscard]] std::uint32_t u32() { return static_cast<std::uint32_t>(take(4)); }
    [[nodiscard]] std::uint64_t u64() { return take(8); }

 private:
    std::span<const std::byte> bytes_;
    std::size_t at_ = 0;
};

// --- Arbitrary bytes -----------------------------------------------------------------

void raw(std::span<const std::byte> bytes) {
    // The reader.
    journal::JournalReader reader(bytes);
    journal::RecordView record;
    std::uint64_t records = 0;
    ReadStatus status = reader.next(record);
    while (status == ReadStatus::Ok) {
        require(record.body != nullptr && record.size == journal::body_size(record.type));
        ++records;
        status = reader.next(record);
    }
    require(reader.next(record) == status);  // final
    require(reader.offset() <= bytes.size());

    // What it vouches for is a clean journal with the same records.
    if (reader.offset() >= journal::kFileHeaderSize) {
        journal::JournalReader again(bytes.first(reader.offset()));
        std::uint64_t reread = 0;
        ReadStatus second = again.next(record);
        while (second == ReadStatus::Ok) {
            ++reread;
            second = again.next(record);
        }
        require(second == ReadStatus::End);
        require(reread == records);
        require(again.next_sequence() == reader.next_sequence());
    } else {
        require(records == 0 && reader.offset() == 0);
        require(status == ReadStatus::TornTail || status == ReadStatus::BadHeader);
    }

    // replay() applies what the reader read. Starting from the file's own
    // first record, so that a journal that does not start at 1 is not refused
    // before anything is read.
    CallLog log;
    const std::uint64_t from =
        reader.offset() >= journal::kFileHeaderSize ? reader.first_sequence() : 1;
    const journal::ReplayResult result = journal::replay(bytes, log, from);
    require(result.applied == records);
    require(result.skipped == 0);
    require(result.good_bytes == reader.offset());
    require(log.after.size() == records);

    // The same bytes as a snapshot.
    if (const std::optional<journal::EngineState> state = journal::decode(bytes)) {
        const std::vector<std::byte> again = journal::encode(*state);
        require(again.size() == bytes.size());
        require(std::memcmp(again.data(), bytes.data(), bytes.size()) == 0);
        static_cast<void>(journal::plausible(*state));
    }
}

// --- Requests built from the input ---------------------------------------------------

void structured(std::span<const std::byte> bytes) {
    Input in(bytes);
    const std::uint8_t cut_seed = in.byte();
    const std::uint16_t damage_at = in.u16();
    const std::uint8_t damage_with = in.byte();

    // The journal, and the same requests made directly.
    std::vector<std::byte> file;
    journal::JournalWriter<AppendTo> writer{AppendTo{&file}};
    CallLog direct;
    std::vector<std::size_t> ends;  // ends[r]: size of the file with r records
    writer.flush();
    ends.push_back(file.size());
    journal::Journaled journaled(direct, writer);
    journal::EngineState state;  // a snapshot's worth, from the same bytes
    while (!in.empty() && ends.size() <= 64) {
        const std::uint8_t kind = in.byte();
        switch (kind % 4) {
            case 0: {
                feed::Symbol symbol;
                for (char& c : symbol.raw) {
                    c = static_cast<char>(in.byte());
                }
                const Locate locate = in.u16();
                journaled.add_instrument(locate, symbol, in.u64());
                state.instruments.push_back({locate, symbol});
                break;
            }
            case 1: {
                engine::NewOrder order;
                order.owner = in.u32();
                order.token = in.u64();
                order.locate = in.u16();
                order.side = static_cast<Side>(static_cast<char>(in.byte()));
                order.qty = in.u32();
                order.price = in.u32();
                order.kind = static_cast<engine::OrderKind>(in.byte());
                order.tif = static_cast<engine::TimeInForce>(in.byte());
                journaled.submit(order, in.u64());
                state.orders.push_back(
                    {order.locate,
                     order.side,
                     {order.token, order.owner, order.token, order.price, order.qty}});
                break;
            }
            case 2: {
                const engine::OwnerId owner = in.u32();
                journaled.cancel(owner, in.u64(), in.u64());
                break;
            }
            default: {
                const engine::OwnerId owner = in.u32();
                const OrderId id = in.u64();
                const Qty qty = in.u32();
                journaled.replace(owner, id, qty, in.u32(), in.u64());
                state.counters.last_order_id = id;
                state.stats.replaces = qty;
                break;
            }
        }
        ends.push_back(file.size());
    }
    const std::uint64_t records = ends.size() - 1;
    require(direct.after.size() == records);
    const auto hash_after = [&direct](std::uint64_t n) {
        return n == 0 ? engine::detail::kHashSeed : direct.after[n - 1];
    };

    // 5. Whole.
    {
        CallLog replayed;
        const journal::ReplayResult result = journal::replay(file, replayed);
        require(result.clean());
        require(result.applied == records);
        require(result.good_bytes == file.size());
        require(result.next_sequence == records + 1);
        require(replayed.hash == direct.hash);
    }

    // 6. Cut short.
    {
        const std::size_t cut = file.size() * cut_seed / 256;
        std::uint64_t whole = 0;
        while (whole + 1 < ends.size() && ends[whole + 1] <= cut) {
            ++whole;
        }
        CallLog replayed;
        const journal::ReplayResult result =
            journal::replay(std::span<const std::byte>(file).first(cut), replayed);
        require(result.usable());
        if (cut >= journal::kFileHeaderSize) {
            require(result.applied == whole);
            require(result.good_bytes == ends[whole]);
            require(result.clean() == (cut == ends[whole]));
            require(replayed.hash == hash_after(whole));
        } else {
            require(result.applied == 0 && result.good_bytes == 0);
        }
    }

    // 7. One byte changed.
    if (damage_with != 0) {
        std::vector<std::byte> damaged = file;
        damaged[damage_at % damaged.size()] ^= static_cast<std::byte>(damage_with);
        CallLog replayed;
        const journal::ReplayResult result = journal::replay(damaged, replayed);
        require(!result.clean());
        require(result.applied <= records);
        require(replayed.hash == hash_after(result.applied));
    }

    // 8. The snapshot.
    {
        state.next_sequence = records + 1;
        const std::vector<std::byte> snapshot = journal::encode(state);
        const std::optional<journal::EngineState> back = journal::decode(snapshot);
        require(back.has_value() && *back == state);
        static_cast<void>(journal::plausible(*back));
        if (damage_with != 0) {
            std::vector<std::byte> damaged = snapshot;
            damaged[damage_at % damaged.size()] ^= static_cast<std::byte>(damage_with);
            require(!journal::decode(damaged).has_value());
        }
        const std::size_t cut = (snapshot.size() - 1) * cut_seed / 255;
        require(!journal::decode(std::span<const std::byte>(snapshot).first(cut)).has_value());
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::span<const std::byte> rest(reinterpret_cast<const std::byte*>(data) + 1, size - 1);
    if ((data[0] & 1U) == 0) {
        raw(rest);
    } else {
        structured(rest);
    }
    return 0;
}
