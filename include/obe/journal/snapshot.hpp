#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/endian.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"
#include "obe/util/crc32.hpp"

// Saving what an engine holds, so that recovery does not have to start from
// the first request ever made.
//
// A journal alone is enough to recover (replay.hpp), and replaying it costs as
// much as the trading it records. A snapshot is the shortcut: everything the
// engine holds at one moment, written out, together with the number of the
// first journal record it does NOT include. Recovery then loads the snapshot
// and replays only the records from that number on.
//
// WHAT HAS TO BE IN IT
//
// The test of a snapshot is that an engine restored from it cannot be told
// apart from the original by anything that happens afterwards. That takes
// more than the book:
//
//   the instruments      which locates are open, and their symbols
//   every resting order  id, owner, token, side, price and open shares, and
//                        its place in the queue, which is kept by listing the
//                        orders of each level oldest first
//   the two counters     the last order id and the last match number. They
//                        are in no order and no level, and without them the
//                        restored engine would hand out ids that are already
//                        in use
//   the statistics       so that the totals carry on instead of restarting
//
// What is NOT in it is anything that can be worked out from those: level
// totals, the best prices, the index from id to order.
//
// A snapshot is taken between two requests, never during one. The engine is
// single-threaded, so that is simply any moment at which it is not inside a
// call.
//
// THE FILE
//
//    0  "OBES"                       4 bytes
//    4  version                      u16, then u16 zero
//    8  next journal sequence        u64
//   16  last order id                u64
//   24  last match number            u64
//   32  the seven statistics         7 x u64, in the order of EngineStats
//   88  number of instruments        u32
//   92  number of orders             u32
//   96  instruments                  each: locate u16, symbol 8 chars
//       orders                       each: locate u16, side 1, id u64,
//                                    owner u32, token u64, price u32, qty u32
//  end  CRC-32 of everything above   u32
//
// A snapshot is written whole and then checked whole. One that fails any
// check is not used at all: unlike a journal, half a snapshot is worth
// nothing, and the journal is still there to recover from.

namespace obe::journal {

inline constexpr char kSnapshotMagic[4] = {'O', 'B', 'E', 'S'};
inline constexpr std::uint16_t kSnapshotVersion = 1;
inline constexpr std::size_t kSnapshotFixedSize = 96;
inline constexpr std::size_t kSnapshotInstrumentSize = 2 + 8;
inline constexpr std::size_t kSnapshotOrderSize = 2 + 1 + 8 + 4 + 8 + 4 + 4;

struct SnapshotInstrument {
    Locate locate = 0;
    feed::Symbol symbol{};

    friend bool operator==(const SnapshotInstrument&, const SnapshotInstrument&) = default;
};

struct SnapshotOrder {
    Locate locate = 0;
    Side side = Side::Buy;
    engine::RestingOrder order;

    friend bool operator==(const SnapshotOrder&, const SnapshotOrder&) = default;
};

// Everything an engine holds, in memory.
struct EngineState {
    // The first journal record this state does not include.
    std::uint64_t next_sequence = 1;
    engine::EngineCounters counters;
    engine::EngineStats stats;
    std::vector<SnapshotInstrument> instruments;  // in order of locate
    // For each instrument, bids then asks; within a side from the best price
    // to the worst; within a price, oldest first. Restoring them in this
    // order puts every order back in its place in its queue.
    std::vector<SnapshotOrder> orders;

    friend bool operator==(const EngineState&, const EngineState&) = default;
};

// Reads the state out of an engine. `next_sequence` is the number the next
// journal record will get, so that recovery knows where to pick the journal
// up. `locates` bounds the walk: locates from 0 up to it are looked at.
template <engine::Restorable Engine>
[[nodiscard]] EngineState capture(const Engine& engine, std::uint64_t next_sequence,
                                  std::uint32_t locates = 65'536) {
    EngineState state;
    state.next_sequence = next_sequence;
    state.counters = engine.counters();
    state.stats = engine.stats();
    state.orders.reserve(engine.open_orders());
    for (std::uint32_t i = 0; i < locates; ++i) {
        const auto locate = static_cast<Locate>(i);
        if (!engine.listed(locate)) {
            continue;
        }
        state.instruments.push_back({locate, engine.symbol(locate)});
        for (const Side side : {Side::Buy, Side::Sell}) {
            engine.for_each_order(locate, side, [&](const engine::RestingOrder& order) {
                state.orders.push_back({locate, side, order});
                return true;
            });
        }
    }
    return state;
}

// Whether a state could have come from an engine. A restored engine is
// trusted completely afterwards, so a state is checked completely before:
//
//   every order is for an instrument in the state, on a real side, with a
//   price and shares;
//   no id appears twice, and none is above the last id the counter says was
//   given out (the engine would give it out again);
//   no book is locked or crossed.
[[nodiscard]] inline bool plausible(const EngineState& state) {
    std::vector<bool> listed(std::size_t{1} << 16, false);
    for (const SnapshotInstrument& instrument : state.instruments) {
        if (listed[instrument.locate]) {
            return false;
        }
        listed[instrument.locate] = true;
    }
    std::unordered_set<OrderId> ids;
    ids.reserve(state.orders.size());
    // The best bid and the best ask of each instrument, as they are met.
    std::vector<Price> best_bid(std::size_t{1} << 16, 0);
    std::vector<Price> best_ask(std::size_t{1} << 16, 0);
    for (const SnapshotOrder& entry : state.orders) {
        const engine::RestingOrder& order = entry.order;
        if (!listed[entry.locate] || !engine::valid_side(entry.side) || order.qty == 0 ||
            order.price == 0 || order.id == 0 || order.id > state.counters.last_order_id ||
            !ids.insert(order.id).second) {
            return false;
        }
        if (entry.side == Side::Buy) {
            best_bid[entry.locate] = std::max(best_bid[entry.locate], order.price);
        } else if (best_ask[entry.locate] == 0 || order.price < best_ask[entry.locate]) {
            best_ask[entry.locate] = order.price;
        }
    }
    for (const SnapshotInstrument& instrument : state.instruments) {
        const Price bid = best_bid[instrument.locate];
        const Price ask = best_ask[instrument.locate];
        if (bid != 0 && ask != 0 && bid >= ask) {
            return false;
        }
    }
    return true;
}

// Loads a state into an engine that has done nothing yet. Publishes and
// reports nothing: the market and the owners were told about all of this when
// it happened.
//
// Returns false if the state is not plausible, in which case the engine has
// not been touched, or if the engine turns out not to be fresh, in which case
// it may be half filled and should be thrown away.
template <engine::Restorable Engine>
[[nodiscard]] bool restore(const EngineState& state, Engine& engine) {
    if (engine.open_orders() != 0 || engine.counters() != engine::EngineCounters{} ||
        !plausible(state)) {
        return false;
    }
    for (const SnapshotInstrument& instrument : state.instruments) {
        if (!engine.restore_instrument(instrument.locate, instrument.symbol)) {
            return false;  // the engine had instruments open: it was not fresh
        }
    }
    for (const SnapshotOrder& entry : state.orders) {
        if (!engine.restore_order(entry.locate, entry.side, entry.order)) {
            return false;
        }
    }
    engine.restore_counters(state.counters, state.stats);
    return true;
}

// The bytes of a snapshot file.
[[nodiscard]] inline std::vector<std::byte> encode(const EngineState& state) {
    std::vector<std::byte> out(kSnapshotFixedSize +
                               state.instruments.size() * kSnapshotInstrumentSize +
                               state.orders.size() * kSnapshotOrderSize + 4);
    std::byte* p = out.data();
    std::memcpy(p, kSnapshotMagic, sizeof(kSnapshotMagic));
    feed::store_be<std::uint16_t>(p + 4, kSnapshotVersion);
    feed::store_be<std::uint16_t>(p + 6, 0);
    feed::store_be<std::uint64_t>(p + 8, state.next_sequence);
    feed::store_be<std::uint64_t>(p + 16, state.counters.last_order_id);
    feed::store_be<std::uint64_t>(p + 24, state.counters.last_match_number);
    const std::uint64_t stats[7] = {state.stats.accepted,       state.stats.rejected,
                                    state.stats.cancels,        state.stats.replaces,
                                    state.stats.trades,         state.stats.traded_shares,
                                    state.stats.unfilled_shares};
    for (std::size_t i = 0; i < 7; ++i) {
        feed::store_be<std::uint64_t>(p + 32 + 8 * i, stats[i]);
    }
    feed::store_be<std::uint32_t>(p + 88, static_cast<std::uint32_t>(state.instruments.size()));
    feed::store_be<std::uint32_t>(p + 92, static_cast<std::uint32_t>(state.orders.size()));
    p += kSnapshotFixedSize;
    for (const SnapshotInstrument& instrument : state.instruments) {
        feed::store_be<std::uint16_t>(p, instrument.locate);
        std::memcpy(p + 2, instrument.symbol.raw.data(), 8);
        p += kSnapshotInstrumentSize;
    }
    for (const SnapshotOrder& entry : state.orders) {
        feed::store_be<std::uint16_t>(p, entry.locate);
        p[2] = static_cast<std::byte>(entry.side);
        feed::store_be<std::uint64_t>(p + 3, entry.order.id);
        feed::store_be<std::uint32_t>(p + 11, entry.order.owner);
        feed::store_be<std::uint64_t>(p + 15, entry.order.token);
        feed::store_be<std::uint32_t>(p + 23, entry.order.price);
        feed::store_be<std::uint32_t>(p + 27, entry.order.qty);
        p += kSnapshotOrderSize;
    }
    const std::uint32_t crc = util::crc32({out.data(), out.size() - 4});
    feed::store_be<std::uint32_t>(p, crc);
    return out;
}

// Reads a snapshot file back. Returns nothing if the bytes are not a whole,
// undamaged snapshot of this version: too short, wrong magic or version, a
// length that does not match the counts inside, or a checksum that does not
// match. Never reads outside `bytes`.
//
// Whatever decodes, encodes back to exactly the bytes it came from: there is
// one file for each state and one state for each file. The fuzz target holds
// it to that.
[[nodiscard]] inline std::optional<EngineState> decode(std::span<const std::byte> bytes) {
    if (bytes.size() < kSnapshotFixedSize + 4) {
        return std::nullopt;
    }
    const std::byte* p = bytes.data();
    if (std::memcmp(p, kSnapshotMagic, sizeof(kSnapshotMagic)) != 0 ||
        feed::load_be<std::uint16_t>(p + 4) != kSnapshotVersion ||
        feed::load_be<std::uint16_t>(p + 6) != 0) {
        return std::nullopt;
    }
    const std::size_t instruments = feed::load_be<std::uint32_t>(p + 88);
    const std::size_t orders = feed::load_be<std::uint32_t>(p + 92);
    // The counts are checked against the length before anything is sized by
    // them: a damaged count must not become a four-gigabyte allocation.
    const std::size_t payload = bytes.size() - kSnapshotFixedSize - 4;
    if (instruments > payload / kSnapshotInstrumentSize || orders > payload / kSnapshotOrderSize ||
        instruments * kSnapshotInstrumentSize + orders * kSnapshotOrderSize != payload) {
        return std::nullopt;
    }
    if (util::crc32({p, bytes.size() - 4}) != feed::load_be<std::uint32_t>(p + bytes.size() - 4)) {
        return std::nullopt;
    }

    EngineState state;
    state.next_sequence = feed::load_be<std::uint64_t>(p + 8);
    state.counters.last_order_id = feed::load_be<std::uint64_t>(p + 16);
    state.counters.last_match_number = feed::load_be<std::uint64_t>(p + 24);
    std::uint64_t stats[7];
    for (std::size_t i = 0; i < 7; ++i) {
        stats[i] = feed::load_be<std::uint64_t>(p + 32 + 8 * i);
    }
    state.stats = {stats[0], stats[1], stats[2], stats[3], stats[4], stats[5], stats[6]};
    p += kSnapshotFixedSize;
    state.instruments.resize(instruments);
    for (SnapshotInstrument& instrument : state.instruments) {
        instrument.locate = feed::load_be<std::uint16_t>(p);
        std::memcpy(instrument.symbol.raw.data(), p + 2, 8);
        p += kSnapshotInstrumentSize;
    }
    state.orders.resize(orders);
    for (SnapshotOrder& entry : state.orders) {
        entry.locate = feed::load_be<std::uint16_t>(p);
        entry.side = static_cast<Side>(static_cast<char>(p[2]));
        entry.order.id = feed::load_be<std::uint64_t>(p + 3);
        entry.order.owner = feed::load_be<std::uint32_t>(p + 11);
        entry.order.token = feed::load_be<std::uint64_t>(p + 15);
        entry.order.price = feed::load_be<std::uint32_t>(p + 23);
        entry.order.qty = feed::load_be<std::uint32_t>(p + 27);
        p += kSnapshotOrderSize;
    }
    return state;
}

}  // namespace obe::journal
