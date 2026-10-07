#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "obe/book/types.hpp"
#include "obe/feed/endian.hpp"
#include "obe/types.hpp"

// A tick, and what it means to be able to write one down.
//
// A tick here is one best-bid-and-offer update: the thing the book publishes
// every time the top of a book changes (book::BboUpdate). A day of Nasdaq has
// on the order of a hundred million of them. The tick store (tick_file.hpp)
// keeps them on disk in blocks; a CODEC decides how the ticks of one block
// become bytes.
//
// THE CONTRACT OF A CODEC
//
//   reset()                     Forget everything. Called at the start of
//                               every block, by the writer and by the reader,
//                               so that a block can be decoded without the
//                               blocks before it.
//   encode(tick, out) -> n      Writes the tick at `out`, which has room for
//                               kMaxTickSize bytes, and returns how many it
//                               used: at least 1, at most kMaxTickSize.
//   decode(p, end, tick) -> n   Reads one tick from [p, end) and returns how
//                               many bytes it took, or 0 if no whole tick is
//                               there. It must not read at or beyond `end`,
//                               whatever the bytes are: they come from a file.
//
// A codec may keep state between calls (the last tick it saw, say) and use it
// to write the next one in fewer bytes. The one rule is that the encoder and
// the decoder keep the SAME state: after encoding ticks 1..k from a reset, and
// after decoding their bytes from a reset, both must be ready for tick k+1 in
// the same way.
//
// It must be lossless for every possible tick, not only for likely ones:
// decode(encode(t)) == t for any timestamps in any order, any locate, any
// price, any quantity up to 2^64 - 1. A codec is free to be bad at unlikely
// ticks. It is not free to be wrong about them.
//
// Two codecs:
//
//   RawCodec     (this file) every field at full width, 34 bytes a tick. The
//                reference: obviously right, and the size everything else is
//                measured against.
//   DeltaCodec   (delta_codec.hpp) the compressing one. Written by hand.

namespace obe::store {

using Tick = book::BboUpdate;

template <class C>
concept TickCodec =
    requires(C codec, const Tick& tick, Tick& out, std::byte* dst, const std::byte* src) {
        { C::kName } -> std::convertible_to<std::string_view>;
        { C::kId } -> std::convertible_to<std::uint16_t>;
        { C::kMaxTickSize } -> std::convertible_to<std::size_t>;
        codec.reset();
        { codec.encode(tick, dst) } -> std::same_as<std::size_t>;
        { codec.decode(src, src, out) } -> std::same_as<std::size_t>;
    };

// Every field as it is, big-endian like the feed:
//
//    0  timestamp   u64
//    8  locate      u16
//   10  bid price   u32
//   14  bid shares  u64
//   22  ask price   u32
//   26  ask shares  u64
//
// It has no state, so reset() has nothing to do, and any tick can be read
// without reading the ones before it.
class RawCodec {
 public:
    static constexpr std::string_view kName = "raw";
    static constexpr std::string_view kDescription = "every field at full width, 34 bytes a tick";
    static constexpr std::uint16_t kId = 1;
    static constexpr std::size_t kMaxTickSize = 34;

    constexpr void reset() noexcept {}

    std::size_t encode(const Tick& tick, std::byte* out) noexcept {
        feed::store_be<std::uint64_t>(out, tick.timestamp);
        feed::store_be<std::uint16_t>(out + 8, tick.locate);
        feed::store_be<std::uint32_t>(out + 10, tick.bbo.bid_price);
        feed::store_be<std::uint64_t>(out + 14, tick.bbo.bid_qty);
        feed::store_be<std::uint32_t>(out + 22, tick.bbo.ask_price);
        feed::store_be<std::uint64_t>(out + 26, tick.bbo.ask_qty);
        return kMaxTickSize;
    }

    [[nodiscard]] std::size_t decode(const std::byte* p, const std::byte* end,
                                     Tick& tick) noexcept {
        if (static_cast<std::size_t>(end - p) < kMaxTickSize) {
            return 0;
        }
        tick.timestamp = feed::load_be<std::uint64_t>(p);
        tick.locate = feed::load_be<std::uint16_t>(p + 8);
        tick.bbo.bid_price = feed::load_be<std::uint32_t>(p + 10);
        tick.bbo.bid_qty = feed::load_be<std::uint64_t>(p + 14);
        tick.bbo.ask_price = feed::load_be<std::uint32_t>(p + 22);
        tick.bbo.ask_qty = feed::load_be<std::uint64_t>(p + 26);
        return kMaxTickSize;
    }
};

static_assert(TickCodec<RawCodec>);

}  // namespace obe::store
