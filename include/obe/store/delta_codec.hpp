#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "obe/store/tick_codec.hpp"
#include "obe/util/huge_pages.hpp"
#include "obe/util/varint.hpp"

// DeltaCodec: the compressing tick codec.
//
// RawCodec writes every tick in 34 bytes. Almost all of those bytes say
// something the reader could have guessed: the time is a few microseconds
// after the last tick, the security is often the one that just ticked, one
// side of its quote has not moved at all and the other moved by a cent or
// changed size by a round lot. DeltaCodec writes only what could not be
// guessed.
//
// The contract is at the top of tick_codec.hpp: lossless for EVERY tick,
// state forgotten at reset(), never a read at or past `end`, never more than
// kMaxTickSize bytes for one tick. The judges:
//
//   tests/store/tick_codec_test.cpp   the contract, on hand-made and random
//                                     ticks, including the hostile ones
//   tests/store/tick_file_test.cpp    whole stores written and read back
//   tests/store/compression_test.cpp  a target: no more than 10 bytes a tick,
//                                     on average, over a generated market
//
// The design was made before any real day was profiled. `tick_store profile
// <ITCH file>` prints what consecutive ticks differ by; on the day of 30
// January 2019 it bore the bets out (35% of ticks repeat the security, 99.5%
// of price changes are whole cents, 84% of size changes whole lots), and the
// codec stored that day at 7.30 bytes a tick (docs/optimization-log.md,
// entry 13).
//
// THE FORMAT OF ONE TICK
//
//   flags      1 byte
//                bit 0  the bid price changed      bit 4  same security as the
//                bit 1  the bid size changed              previous tick
//                bit 2  the ask price changed      bit 5  bid price change in cents
//                bit 3  the ask size changed       bit 6  ask price change in cents
//                                                  bit 7  size changes in round lots
//   time       varint, zigzag of the difference from the previous tick's time
//   locate     2 bytes, big-endian; only when bit 4 is clear
//   then, for each field whose bit is set, in the order of the bits:
//              varint, zigzag of the difference from that security's last
//              value of the field, divided by 100 when bit 5, 6 or 7 says so
//
// DESIGN
//
//  1. Two kinds of "previous".
//     The time and the security are compared with the tick just before in
//     the block: ticks come in time order, so the gap is small, and bursts
//     on one security are common. The quote fields are compared with the
//     last quote of the SAME security, because the tick before is usually
//     another stock entirely and its prices say nothing about this one.
//
//  2. Per-security memory that reset() does not touch.
//     The last quote of every locate is a table of 65536 entries of 32 bytes
//     (2 MiB). reset() runs at the start of every block; clearing the table
//     there would write 2 MiB for every few thousand ticks. Instead each
//     entry carries the number of the block it was written in (an epoch):
//     reset() increments the codec's epoch, and an entry from an older epoch
//     reads as empty. reset() is then two stores. The table is an anonymous
//     memory mapping, which the system hands over as zero pages on first
//     touch: constructing a codec (a reader makes one per query) costs one
//     mapping and no 2 MiB write, and a query for one block only faults in
//     the pages of the securities that block mentions.
//
//  3. The first tick of a security in a block needs no special case.
//     Its entry reads as empty: every field zero. The difference from zero
//     is the value itself, which is exactly what has to be written.
//
//  4. Signed differences of unsigned fields: wrap, then zigzag.
//     new - old computed in unsigned arithmetic wraps modulo 2^64, and adding
//     it back the same way on the other side restores new exactly, for every
//     pair of values: nothing is lost. A small decrease wraps to a huge
//     number, though, which would take ten bytes as a varint. Read as a
//     signed number it is small and negative, and zigzag maps small
//     magnitudes of either sign to small unsigned numbers (0, -1, 1, -2 ...
//     become 0, 1, 2, 3 ...), so a change of one cent either way fits in a
//     byte or two.
//
//  5. What did not change costs one bit.
//     The flags byte says which of the four quote fields changed; most ticks
//     change one or two. That takes four bits. The other four say the most
//     valuable things left: "same security as the tick before" (saves the
//     two locate bytes), and "this difference is a whole number of cents" or
//     "of round lots" (point 6).
//
//  6. Cents and round lots.
//     Prices are in 1/10000 of a dollar, so a one-cent move is 100: zigzag
//     200, two varint bytes. Divided by 100 it is 1: one byte. When a price
//     difference is a whole number of cents (the normal case) it is written
//     in cents and its bit says so; a sub-penny difference is written as it
//     is, with the bit clear, so sub-penny stocks lose nothing but the
//     saving. Sizes move in round lots of 100 shares just as often, and one
//     shared bit says "every size difference in this tick is in lots". The
//     decoder multiplies back with unsigned arithmetic, so even a damaged
//     byte cannot cause an overflow.
//
//  7. The worst case: 1 flag byte, 10 for the time, 2 for the locate and 10
//     for each of four fields: 53 bytes. kMaxTickSize is 64.
//
//  8. Reading bytes it did not write.
//     Every read goes through get_varint, which is told where `end` is and
//     accepts only the shortest encoding, or through an explicit check that
//     two bytes remain. A flags byte the encoder never writes (a "cents" bit
//     for a price that did not change, the lots bit with no size change, the
//     same-security bit on the first tick of a block) is refused. That
//     catches some damage early; the block checksum catches the rest.
//
//  9. Against a general-purpose compressor: see the log entry. zstd or xz on
//     the raw store has a window of megabytes and no idea what a tick is;
//     this codec knows exactly what a tick is and looks back one tick per
//     security. Which wins on size, and at what speed, is a measurement.

namespace obe::store {

class DeltaCodec {
 public:
    static constexpr std::string_view kName = "delta";
    static constexpr std::string_view kDescription =
        "differences from the last tick, in variable-length integers, cents and round lots";
    static constexpr std::uint16_t kId = 2;
    // The most bytes one tick may take (design point 7).
    static constexpr std::size_t kMaxTickSize = 64;

    DeltaCodec() : table_(sizeof(Entry) * kLocates, /*request_huge=*/false) {}

    void reset() noexcept {
        ++epoch_;
        prev_time_ = 0;
        prev_locate_ = kNoLocate;
    }

    std::size_t encode(const Tick& tick, std::byte* out) {
        Entry& e = entry(tick.locate);
        const std::uint64_t d_bid_price = diff(tick.bbo.bid_price, e.bid_price);
        const std::uint64_t d_bid_qty = diff(tick.bbo.bid_qty, e.bid_qty);
        const std::uint64_t d_ask_price = diff(tick.bbo.ask_price, e.ask_price);
        const std::uint64_t d_ask_qty = diff(tick.bbo.ask_qty, e.ask_qty);

        unsigned flags = 0;
        if (d_bid_price != 0) {
            flags |= kBidPrice | (whole(d_bid_price) ? kBidCents : 0U);
        }
        if (d_bid_qty != 0) {
            flags |= kBidQty;
        }
        if (d_ask_price != 0) {
            flags |= kAskPrice | (whole(d_ask_price) ? kAskCents : 0U);
        }
        if (d_ask_qty != 0) {
            flags |= kAskQty;
        }
        if ((flags & (kBidQty | kAskQty)) != 0 && (d_bid_qty == 0 || whole(d_bid_qty)) &&
            (d_ask_qty == 0 || whole(d_ask_qty))) {
            flags |= kLots;
        }
        if (tick.locate == prev_locate_) {
            flags |= kSameLocate;
        }

        std::size_t n = 0;
        out[n++] = static_cast<std::byte>(flags);
        n += put(diff(tick.timestamp, prev_time_), out + n);
        if ((flags & kSameLocate) == 0) {
            out[n++] = static_cast<std::byte>(tick.locate >> 8);
            out[n++] = static_cast<std::byte>(tick.locate & 0xFF);
        }
        if ((flags & kBidPrice) != 0) {
            n += put(scaled(d_bid_price, (flags & kBidCents) != 0), out + n);
        }
        if ((flags & kBidQty) != 0) {
            n += put(scaled(d_bid_qty, (flags & kLots) != 0), out + n);
        }
        if ((flags & kAskPrice) != 0) {
            n += put(scaled(d_ask_price, (flags & kAskCents) != 0), out + n);
        }
        if ((flags & kAskQty) != 0) {
            n += put(scaled(d_ask_qty, (flags & kLots) != 0), out + n);
        }

        remember(e, tick);
        return n;
    }

    [[nodiscard]] std::size_t decode(const std::byte* p, const std::byte* end, Tick& tick) {
        const std::byte* const start = p;
        if (p >= end) {
            return 0;
        }
        const auto flags = static_cast<unsigned>(*p++);
        if (!canonical(flags)) {
            return 0;
        }

        std::uint64_t v = 0;
        if (!get(p, end, v)) {
            return 0;
        }
        tick.timestamp = prev_time_ + v;
        if ((flags & kSameLocate) != 0) {
            if (prev_locate_ == kNoLocate) {
                return 0;  // nothing came before it in this block
            }
            tick.locate = static_cast<Locate>(prev_locate_);
        } else {
            if (end - p < 2) {
                return 0;
            }
            tick.locate = static_cast<Locate>((static_cast<unsigned>(p[0]) << 8) |
                                              static_cast<unsigned>(p[1]));
            p += 2;
        }

        Entry& e = entry(tick.locate);
        std::uint64_t bid_price = e.bid_price;
        std::uint64_t ask_price = e.ask_price;
        tick.bbo.bid_qty = e.bid_qty;
        tick.bbo.ask_qty = e.ask_qty;
        if ((flags & kBidPrice) != 0) {
            if (!get(p, end, v)) {
                return 0;
            }
            bid_price += unscaled(v, (flags & kBidCents) != 0);
        }
        if ((flags & kBidQty) != 0) {
            if (!get(p, end, v)) {
                return 0;
            }
            tick.bbo.bid_qty += unscaled(v, (flags & kLots) != 0);
        }
        if ((flags & kAskPrice) != 0) {
            if (!get(p, end, v)) {
                return 0;
            }
            ask_price += unscaled(v, (flags & kAskCents) != 0);
        }
        if ((flags & kAskQty) != 0) {
            if (!get(p, end, v)) {
                return 0;
            }
            tick.bbo.ask_qty += unscaled(v, (flags & kLots) != 0);
        }
        // Prices are 32 bits; the differences were taken modulo 2^64 of the
        // 32-bit values, so the low 32 bits are the price.
        tick.bbo.bid_price = static_cast<Price>(bid_price);
        tick.bbo.ask_price = static_cast<Price>(ask_price);

        remember(e, tick);
        return static_cast<std::size_t>(p - start);
    }

 private:
    // The last quote of one security, and the block it was written in.
    struct Entry {
        Price bid_price;
        Price ask_price;
        std::uint64_t bid_qty;
        std::uint64_t ask_qty;
        std::uint64_t epoch;  // 0 in a fresh mapping: never the current one
    };
    static_assert(sizeof(Entry) == 32);

    static constexpr std::size_t kLocates = std::size_t{1} << 16;
    static constexpr std::uint32_t kNoLocate = 0x10000;  // no tick yet in this block

    static constexpr unsigned kBidPrice = 1U << 0;
    static constexpr unsigned kBidQty = 1U << 1;
    static constexpr unsigned kAskPrice = 1U << 2;
    static constexpr unsigned kAskQty = 1U << 3;
    static constexpr unsigned kSameLocate = 1U << 4;
    static constexpr unsigned kBidCents = 1U << 5;
    static constexpr unsigned kAskCents = 1U << 6;
    static constexpr unsigned kLots = 1U << 7;

    static constexpr std::uint64_t kScale = 100;  // a cent, and a round lot

    // A scale bit only for a field that changed (design point 8).
    [[nodiscard]] static constexpr bool canonical(unsigned flags) noexcept {
        if ((flags & kBidCents) != 0 && (flags & kBidPrice) == 0) {
            return false;
        }
        if ((flags & kAskCents) != 0 && (flags & kAskPrice) == 0) {
            return false;
        }
        if ((flags & kLots) != 0 && (flags & (kBidQty | kAskQty)) == 0) {
            return false;
        }
        return true;
    }

    // new - old, modulo 2^64 (design point 4).
    [[nodiscard]] static constexpr std::uint64_t diff(std::uint64_t now,
                                                      std::uint64_t before) noexcept {
        return now - before;
    }

    // Whether a wrapped difference, read as signed, is a whole number of
    // cents or lots.
    [[nodiscard]] static constexpr bool whole(std::uint64_t d) noexcept {
        return static_cast<std::int64_t>(d) % static_cast<std::int64_t>(kScale) == 0;
    }

    [[nodiscard]] static constexpr std::uint64_t scaled(std::uint64_t d, bool in_units) noexcept {
        if (!in_units) {
            return d;
        }
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(d) /
                                          static_cast<std::int64_t>(kScale));
    }

    // From what get() read back to a wrapped difference. Unsigned
    // multiplication: a damaged value wraps instead of overflowing.
    [[nodiscard]] static constexpr std::uint64_t unscaled(std::uint64_t d, bool in_units) noexcept {
        return in_units ? d * kScale : d;
    }

    static std::size_t put(std::uint64_t wrapped, std::byte* out) noexcept {
        return util::put_varint(util::zigzag(static_cast<std::int64_t>(wrapped)), out);
    }

    // Reads one zigzag varint into `wrapped` and advances p. False if no
    // valid varint is there.
    static bool get(const std::byte*& p, const std::byte* end, std::uint64_t& wrapped) noexcept {
        std::uint64_t v = 0;
        const std::size_t n = util::get_varint(p, end, v);
        if (n == 0) {
            return false;
        }
        p += n;
        wrapped = static_cast<std::uint64_t>(util::unzigzag(v));
        return true;
    }

    // The entry for `locate`, emptied first if it belongs to an earlier block.
    Entry& entry(Locate locate) noexcept {
        Entry& e = reinterpret_cast<Entry*>(table_.data())[locate];
        if (e.epoch != epoch_) {
            e = Entry{0, 0, 0, 0, epoch_};
        }
        return e;
    }

    void remember(Entry& e, const Tick& tick) noexcept {
        e.bid_price = tick.bbo.bid_price;
        e.ask_price = tick.bbo.ask_price;
        e.bid_qty = tick.bbo.bid_qty;
        e.ask_qty = tick.bbo.ask_qty;
        prev_time_ = tick.timestamp;
        prev_locate_ = tick.locate;
    }

    util::HugePageBuffer table_;  // kLocates entries, zero until touched
    std::uint64_t epoch_ = 1;
    Nanos prev_time_ = 0;
    std::uint32_t prev_locate_ = kNoLocate;
};

static_assert(TickCodec<DeltaCodec>);

}  // namespace obe::store
