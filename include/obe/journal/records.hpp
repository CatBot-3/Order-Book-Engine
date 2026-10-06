#pragma once

#include <cstddef>
#include <cstdint>

#include "obe/engine/types.hpp"
#include "obe/feed/codec.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"

// What the journal records, and how it is laid out in the file.
//
// THE IDEA
//
// The matching engine never reads a clock and never makes a random choice:
// what it does is decided entirely by the requests it is given, their order
// and the times they carry (obe/engine/concepts.hpp). So its state does not
// have to be saved to survive a crash. It is enough to save the requests. Run
// them again, in order, through a fresh engine, and it arrives at the state
// the old one was in and says exactly what the old one said.
//
// That is why the journal holds inputs and nothing else: no trades, no book,
// no reports. Four kinds of record, one for each thing an engine can be asked
// to do.
//
// It is also why the journal is written BEFORE the engine acts ("write-ahead").
// If the engine acted first and the process died before the request was
// recorded, the world might already have seen a trade that no replay will
// ever produce again.
//
// THE FILE
//
//   file header   16 bytes   "OBEJ", version (u16), zero (u16),
//                            sequence number of the first record (u64)
//   record        14 bytes   crc (u32)   CRC-32 of everything after this field
//                            size (u16)  bytes in the body
//                            seq (u64)   1, 2, 3, ... with no gaps
//                 size bytes body: a type byte, then that record's fields
//
// All integers are big-endian, like every other format in this project.
//
// The checksum is what lets a reader tell a whole record from one that was
// cut off when the machine stopped, which is the normal way for a journal to
// end. The sequence number catches what a checksum cannot: a record that is
// intact but missing, repeated or from another file.

namespace obe::journal {

inline constexpr std::size_t kFileHeaderSize = 16;
inline constexpr std::size_t kRecordHeaderSize = 14;
inline constexpr std::uint16_t kVersion = 1;
inline constexpr char kMagic[4] = {'O', 'B', 'E', 'J'};

// 'I': an instrument is opened.
struct AddInstrument {
    static constexpr char kType = 'I';
    Nanos now = 0;
    Locate locate = 0;
    feed::Symbol symbol{};

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.now);
        v.u16(m.locate);
        v.chars(m.symbol.raw);
    }
    friend bool operator==(const AddInstrument&, const AddInstrument&) = default;
};

// 'S': an order is submitted.
//
// Every field is stored as it was given, valid or not. A side byte that is
// neither 'B' nor 'S' is an input the engine rejects, and the rejection is
// part of what a replay has to reproduce, so the journal must not tidy it up.
// The same goes for the kind and the time in force: they are kept as the raw
// byte behind the enum.
struct Submit {
    static constexpr char kType = 'S';
    Nanos now = 0;
    engine::OwnerId owner = 0;
    engine::Token token = 0;
    Locate locate = 0;
    Side side = Side::Buy;
    Qty qty = 0;
    Price price = 0;
    char kind = 0;  // the byte behind engine::OrderKind
    char tif = 0;   // the byte behind engine::TimeInForce

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.now);
        v.u32(m.owner);
        v.u64(m.token);
        v.u16(m.locate);
        v.side(m.side);
        v.u32(m.qty);
        v.u32(m.price);
        v.ch(m.kind);
        v.ch(m.tif);
    }
    friend bool operator==(const Submit&, const Submit&) = default;

    [[nodiscard]] static constexpr Submit from(const engine::NewOrder& order, Nanos now) noexcept {
        return {.now = now,
                .owner = order.owner,
                .token = order.token,
                .locate = order.locate,
                .side = order.side,
                .qty = order.qty,
                .price = order.price,
                .kind = static_cast<char>(order.kind),
                .tif = static_cast<char>(order.tif)};
    }
    [[nodiscard]] constexpr engine::NewOrder order() const noexcept {
        return {.owner = owner,
                .token = token,
                .locate = locate,
                .side = side,
                .qty = qty,
                .price = price,
                .kind = static_cast<engine::OrderKind>(static_cast<std::uint8_t>(kind)),
                .tif = static_cast<engine::TimeInForce>(static_cast<std::uint8_t>(tif))};
    }
};

// 'X': a cancel is requested.
struct Cancel {
    static constexpr char kType = 'X';
    Nanos now = 0;
    engine::OwnerId owner = 0;
    OrderId order_id = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.now);
        v.u32(m.owner);
        v.u64(m.order_id);
    }
    friend bool operator==(const Cancel&, const Cancel&) = default;
};

// 'U': a replace is requested.
struct Replace {
    static constexpr char kType = 'U';
    Nanos now = 0;
    engine::OwnerId owner = 0;
    OrderId order_id = 0;
    Qty qty = 0;
    Price price = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        v.u64(m.now);
        v.u32(m.owner);
        v.u64(m.order_id);
        v.u32(m.qty);
        v.u32(m.price);
    }
    friend bool operator==(const Replace&, const Replace&) = default;
};

// The sizes of the bodies, type byte included. If a field list above gains,
// loses or mis-sizes a field, the build stops here, and so does every journal
// written before the change: bump kVersion with it.
inline constexpr std::size_t kAddInstrumentSize = feed::kWireSize<AddInstrument>;
inline constexpr std::size_t kSubmitSize = feed::kWireSize<Submit>;
inline constexpr std::size_t kCancelSize = feed::kWireSize<Cancel>;
inline constexpr std::size_t kReplaceSize = feed::kWireSize<Replace>;
static_assert(kAddInstrumentSize == 1 + 8 + 2 + 8);
static_assert(kSubmitSize == 1 + 8 + 4 + 8 + 2 + 1 + 4 + 4 + 1 + 1);
static_assert(kCancelSize == 1 + 8 + 4 + 8);
static_assert(kReplaceSize == 1 + 8 + 4 + 8 + 4 + 4);

inline constexpr std::size_t kMaxBodySize = kSubmitSize;

// The size of the body a record of this type must have, or 0 if there is no
// such type.
[[nodiscard]] constexpr std::size_t body_size(char type) noexcept {
    switch (type) {
        case AddInstrument::kType:
            return kAddInstrumentSize;
        case Submit::kType:
            return kSubmitSize;
        case Cancel::kType:
            return kCancelSize;
        case Replace::kType:
            return kReplaceSize;
        default:
            return 0;
    }
}

}  // namespace obe::journal
