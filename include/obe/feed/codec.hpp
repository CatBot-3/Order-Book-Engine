#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "obe/feed/endian.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"

// Turning bytes into message structs and back.
//
// Each message struct lists its fields once (visit() in messages.hpp). Three
// small visitors walk that list: Reader decodes, Writer encodes, Sizer counts
// bytes. The visitors are templates resolved at compile time, so decode<M>()
// compiles down to a handful of loads at fixed offsets, the same code as a
// hand-written decoder.
//
// decode() does no bounds checking. The framing layer (parser.hpp) is the
// single place that proves `wire_size` bytes are readable before any field is
// touched.

namespace obe::feed {

namespace detail {

struct Reader {
    const std::byte* p;

    void ch(char& c) noexcept {
        c = static_cast<char>(*p);
        p += 1;
    }
    void side(Side& s) noexcept {
        s = static_cast<Side>(static_cast<char>(*p));
        p += 1;
    }
    void u16(std::uint16_t& x) noexcept {
        x = load_be<std::uint16_t>(p);
        p += 2;
    }
    void u32(std::uint32_t& x) noexcept {
        x = load_be<std::uint32_t>(p);
        p += 4;
    }
    void u48(std::uint64_t& x) noexcept {
        x = load_be48(p);
        p += 6;
    }
    void u64(std::uint64_t& x) noexcept {
        x = load_be<std::uint64_t>(p);
        p += 8;
    }
    template <std::size_t N>
    void chars(std::array<char, N>& a) noexcept {
        std::memcpy(a.data(), p, N);
        p += N;
    }
};

struct Writer {
    std::byte* p;

    void ch(char c) noexcept {
        *p = static_cast<std::byte>(c);
        p += 1;
    }
    void side(Side s) noexcept { ch(static_cast<char>(s)); }
    void u16(std::uint16_t x) noexcept {
        store_be<std::uint16_t>(p, x);
        p += 2;
    }
    void u32(std::uint32_t x) noexcept {
        store_be<std::uint32_t>(p, x);
        p += 4;
    }
    void u48(std::uint64_t x) noexcept {
        store_be48(p, x);
        p += 6;
    }
    void u64(std::uint64_t x) noexcept {
        store_be<std::uint64_t>(p, x);
        p += 8;
    }
    template <std::size_t N>
    void chars(const std::array<char, N>& a) noexcept {
        std::memcpy(p, a.data(), N);
        p += N;
    }
};

struct Sizer {
    std::size_t n = 0;

    constexpr void ch(char) noexcept { n += 1; }
    constexpr void side(Side) noexcept { n += 1; }
    constexpr void u16(std::uint16_t) noexcept { n += 2; }
    constexpr void u32(std::uint32_t) noexcept { n += 4; }
    constexpr void u48(std::uint64_t) noexcept { n += 6; }
    constexpr void u64(std::uint64_t) noexcept { n += 8; }
    template <std::size_t N>
    constexpr void chars(const std::array<char, N>&) noexcept {
        n += N;
    }
};

}  // namespace detail

// Bytes this message occupies on the wire, including the type byte and
// excluding any length prefix.
template <class M>
[[nodiscard]] constexpr std::size_t wire_size(const M& m) noexcept {
    detail::Sizer s;
    M::visit(m, s);
    return s.n + 1;
}

template <class M>
inline constexpr std::size_t kWireSize = wire_size(M{});

// The sizes below are the ones in the Nasdaq specification. If a field list in
// messages.hpp gains, loses or mis-sizes a field, the build stops here.
static_assert(kWireSize<SystemEvent> == 12);
static_assert(kWireSize<StockDirectory> == 39);
static_assert(kWireSize<TradingAction> == 25);
static_assert(kWireSize<RegSho> == 20);
static_assert(kWireSize<MarketParticipantPosition> == 26);
static_assert(kWireSize<MwcbDeclineLevel> == 35);
static_assert(kWireSize<MwcbStatus> == 12);
static_assert(kWireSize<IpoQuotingPeriod> == 28);
static_assert(kWireSize<LuldAuctionCollar> == 35);
static_assert(kWireSize<OperationalHalt> == 21);
static_assert(kWireSize<AddOrder> == 36);
static_assert(wire_size(AddOrder{.attributed = true}) == 40);
static_assert(kWireSize<OrderExecuted> == 31);
static_assert(kWireSize<OrderExecutedWithPrice> == 36);
static_assert(kWireSize<OrderCancel> == 23);
static_assert(kWireSize<OrderDelete> == 19);
static_assert(kWireSize<OrderReplace> == 35);
static_assert(kWireSize<Trade> == 44);
static_assert(kWireSize<CrossTrade> == 40);
static_assert(kWireSize<BrokenTrade> == 19);
static_assert(kWireSize<Noii> == 50);
static_assert(kWireSize<Rpii> == 20);
static_assert(kWireSize<DirectListingCapitalRaise> == 48);

inline constexpr std::size_t kMaxMessageSize = 50;

// Decode a message of type M starting at its type byte.
// Precondition: wire_size bytes are readable at p, and p[0] is M's type.
template <class M>
[[nodiscard]] inline M decode(const std::byte* p) noexcept {
    M m;
    if constexpr (requires { m.attributed; }) {
        m.attributed = static_cast<char>(p[0]) == M::kTypeAttributed;
    }
    detail::Reader r{p + 1};
    M::visit(m, r);
    return m;
}

// Encode m at out, type byte first. Returns the number of bytes written.
// Precondition: wire_size(m) bytes are writable at out.
template <class M>
inline std::size_t encode(const M& m, std::byte* out) noexcept {
    out[0] = static_cast<std::byte>(m.type());
    detail::Writer w{out + 1};
    M::visit(m, w);
    return static_cast<std::size_t>(w.p - out);
}

// Append m to a stream in the layout of Nasdaq's sample files: a two-byte
// big-endian length, then the message.
template <class M>
inline void append_framed(std::vector<std::byte>& out, const M& m) {
    const std::size_t size = wire_size(m);
    const std::size_t at = out.size();
    out.resize(at + 2 + size);
    store_be<std::uint16_t>(out.data() + at, static_cast<std::uint16_t>(size));
    encode(m, out.data() + at + 2);
}

}  // namespace obe::feed
