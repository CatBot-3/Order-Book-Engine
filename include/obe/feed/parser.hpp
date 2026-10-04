#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "obe/feed/codec.hpp"
#include "obe/feed/endian.hpp"
#include "obe/feed/handler.hpp"
#include "obe/feed/messages.hpp"

// Framing and dispatch for an ITCH 5.0 stream.
//
// Two layers, kept apart on purpose:
//
//   FrameReader   walks the two-byte length prefixes of a file-style stream
//                 and proves each message lies inside the buffer.
//   ItchParser    takes one message, checks its length against its type, and
//                 calls the matching handler callback.
//
// A UDP packet (phase 7) carries the same messages with different framing, so
// it will reuse ItchParser::dispatch and replace only the FrameReader.
//
// The input is untrusted. Every way a stream can be wrong ends in a
// ParseStatus, never in a read outside the buffer.

namespace obe::feed {

enum class ParseStatus : std::uint8_t {
    Ok,
    TruncatedLength,   // fewer than two bytes left where a length prefix should be
    TruncatedMessage,  // the length prefix promises more bytes than remain
    UnknownType,       // the type byte is not an ITCH 5.0 message type
    LengthMismatch,    // the length is not the one the specification gives for this type
};

[[nodiscard]] constexpr std::string_view to_string(ParseStatus s) noexcept {
    switch (s) {
        case ParseStatus::Ok:
            return "ok";
        case ParseStatus::TruncatedLength:
            return "truncated length prefix";
        case ParseStatus::TruncatedMessage:
            return "truncated message";
        case ParseStatus::UnknownType:
            return "unknown message type";
        case ParseStatus::LengthMismatch:
            return "length does not match message type";
    }
    return "invalid status";
}

struct ParseResult {
    ParseStatus status = ParseStatus::Ok;
    std::uint64_t messages = 0;  // messages delivered to the handler
    std::size_t offset = 0;      // on error: offset of the bad frame's length prefix.
                                 // on success: the buffer size.

    [[nodiscard]] constexpr bool ok() const noexcept { return status == ParseStatus::Ok; }
};

// The wire size the specification gives for a type byte, or 0 if the byte is
// not a message type.
[[nodiscard]] constexpr std::size_t message_size(char type) noexcept {
    switch (type) {
        case 'S':
            return 12;
        case 'R':
            return 39;
        case 'H':
            return 25;
        case 'Y':
            return 20;
        case 'L':
            return 26;
        case 'V':
            return 35;
        case 'W':
            return 12;
        case 'K':
            return 28;
        case 'J':
            return 35;
        case 'h':
            return 21;
        case 'A':
            return 36;
        case 'F':
            return 40;
        case 'E':
            return 31;
        case 'C':
            return 36;
        case 'X':
            return 23;
        case 'D':
            return 19;
        case 'U':
            return 35;
        case 'P':
            return 44;
        case 'Q':
            return 40;
        case 'B':
            return 19;
        case 'I':
            return 50;
        case 'N':
            return 20;
        case 'O':
            return 48;
        default:
            return 0;
    }
}

// Every type byte, in the order the specification lists them.
inline constexpr std::array<char, 23> kMessageTypes{'S', 'R', 'H', 'Y', 'L', 'V', 'W', 'K',
                                                    'J', 'h', 'A', 'F', 'E', 'C', 'X', 'D',
                                                    'U', 'P', 'Q', 'B', 'I', 'N', 'O'};

// One message inside a buffer: `data` points at the type byte.
struct Frame {
    const std::byte* data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] char type() const noexcept { return static_cast<char>(data[0]); }
};

// Bytes 0 to 10 are the same in every message, so the timestamp can be read
// without decoding the rest. Precondition: f.size >= 11, which holds for any
// frame that ItchParser::dispatch accepted or would accept.
[[nodiscard]] inline Nanos peek_timestamp(const Frame& f) noexcept {
    return load_be48(f.data + 5);
}
[[nodiscard]] inline Locate peek_locate(const Frame& f) noexcept {
    return load_be<Locate>(f.data + 1);
}

inline constexpr std::size_t kLengthPrefixSize = 2;

// Walks a stream of [2-byte big-endian length][message] records.
class FrameReader {
 public:
    explicit FrameReader(std::span<const std::byte> buf) noexcept : buf_(buf) {}

    [[nodiscard]] bool done() const noexcept { return pos_ == buf_.size(); }

    // Offset of the next length prefix.
    [[nodiscard]] std::size_t offset() const noexcept { return pos_; }

    // Precondition: !done(). On Ok, `out` is the next message and the reader
    // has moved past it. On any other status the reader has not moved.
    [[nodiscard]] ParseStatus next(Frame& out) noexcept {
        const std::size_t remaining = buf_.size() - pos_;
        if (remaining < kLengthPrefixSize) [[unlikely]] {
            return ParseStatus::TruncatedLength;
        }
        const std::size_t len = load_be<std::uint16_t>(buf_.data() + pos_);
        if (len > remaining - kLengthPrefixSize) [[unlikely]] {
            return ParseStatus::TruncatedMessage;
        }
        out.data = buf_.data() + pos_ + kLengthPrefixSize;
        out.size = len;
        pos_ += kLengthPrefixSize + len;
        return ParseStatus::Ok;
    }

 private:
    std::span<const std::byte> buf_;
    std::size_t pos_ = 0;
};

template <ItchHandler H>
class ItchParser {
 public:
    explicit ItchParser(H& handler) noexcept : h_(&handler) {}

    // Decode one message (type byte first, no length prefix) and call the
    // matching callback. `len` must be the exact size the specification gives
    // for the message's type; anything else is rejected before a field is read.
    ParseStatus dispatch(const std::byte* msg, std::size_t len) {
        if (len == 0) [[unlikely]] {
            return ParseStatus::LengthMismatch;
        }
        H& h = *h_;
        switch (static_cast<char>(msg[0])) {
            case 'A':
                return deliver<AddOrder>(msg, len, [&h](const AddOrder& m) { h.on_add(m); });
            case 'F':
                return deliver<AddOrder>(
                    msg, len, [&h](const AddOrder& m) { h.on_add(m); }, kAttributedAddSize);
            case 'E':
                return deliver<OrderExecuted>(msg, len,
                                              [&h](const OrderExecuted& m) { h.on_execute(m); });
            case 'C':
                return deliver<OrderExecutedWithPrice>(
                    msg, len,
                    [&h](const OrderExecutedWithPrice& m) { h.on_execute_with_price(m); });
            case 'X':
                return deliver<OrderCancel>(msg, len,
                                            [&h](const OrderCancel& m) { h.on_cancel(m); });
            case 'D':
                return deliver<OrderDelete>(msg, len,
                                            [&h](const OrderDelete& m) { h.on_delete(m); });
            case 'U':
                return deliver<OrderReplace>(msg, len,
                                             [&h](const OrderReplace& m) { h.on_replace(m); });
            case 'P':
                return deliver<Trade>(msg, len, [&h](const Trade& m) { h.on_trade(m); });
            case 'Q':
                return deliver<CrossTrade>(msg, len,
                                           [&h](const CrossTrade& m) { h.on_cross_trade(m); });
            case 'B':
                return deliver<BrokenTrade>(msg, len,
                                            [&h](const BrokenTrade& m) { h.on_broken_trade(m); });
            case 'I':
                return deliver<Noii>(msg, len, [&h](const Noii& m) { h.on_noii(m); });
            case 'N':
                return deliver<Rpii>(msg, len, [&h](const Rpii& m) { h.on_rpii(m); });
            case 'S':
                return deliver<SystemEvent>(msg, len,
                                            [&h](const SystemEvent& m) { h.on_system_event(m); });
            case 'R':
                return deliver<StockDirectory>(
                    msg, len, [&h](const StockDirectory& m) { h.on_stock_directory(m); });
            case 'H':
                return deliver<TradingAction>(
                    msg, len, [&h](const TradingAction& m) { h.on_trading_action(m); });
            case 'Y':
                return deliver<RegSho>(msg, len, [&h](const RegSho& m) { h.on_reg_sho(m); });
            case 'L':
                return deliver<MarketParticipantPosition>(msg, len,
                                                          [&h](const MarketParticipantPosition& m) {
                                                              h.on_market_participant_position(m);
                                                          });
            case 'V':
                return deliver<MwcbDeclineLevel>(
                    msg, len, [&h](const MwcbDeclineLevel& m) { h.on_mwcb_decline_level(m); });
            case 'W':
                return deliver<MwcbStatus>(msg, len,
                                           [&h](const MwcbStatus& m) { h.on_mwcb_status(m); });
            case 'K':
                return deliver<IpoQuotingPeriod>(
                    msg, len, [&h](const IpoQuotingPeriod& m) { h.on_ipo_quoting_period(m); });
            case 'J':
                return deliver<LuldAuctionCollar>(
                    msg, len, [&h](const LuldAuctionCollar& m) { h.on_luld_auction_collar(m); });
            case 'h':
                return deliver<OperationalHalt>(
                    msg, len, [&h](const OperationalHalt& m) { h.on_operational_halt(m); });
            case 'O':
                return deliver<DirectListingCapitalRaise>(
                    msg, len, [&h](const DirectListingCapitalRaise& m) { h.on_direct_listing(m); });
            default:
                return ParseStatus::UnknownType;
        }
    }

    ParseStatus dispatch(const Frame& f) { return dispatch(f.data, f.size); }

    // Parse a whole file-style stream. Stops at the first problem and reports
    // where it was; everything before that point has been delivered.
    ParseResult parse(std::span<const std::byte> buf) {
        FrameReader reader(buf);
        ParseResult result;
        Frame frame;
        while (!reader.done()) {
            result.offset = reader.offset();
            result.status = reader.next(frame);
            if (result.status != ParseStatus::Ok) [[unlikely]] {
                return result;
            }
            result.status = dispatch(frame.data, frame.size);
            if (result.status != ParseStatus::Ok) [[unlikely]] {
                return result;
            }
            ++result.messages;
        }
        result.offset = buf.size();
        return result;
    }

 private:
    static constexpr std::size_t kAttributedAddSize = 40;

    // `expected` is a compile-time constant at every call site, so the length
    // check is one compare against an immediate.
    template <class M, class F>
    static ParseStatus deliver(const std::byte* msg, std::size_t len, F&& call,
                               std::size_t expected = kWireSize<M>) {
        if (len != expected) [[unlikely]] {
            return ParseStatus::LengthMismatch;
        }
        call(decode<M>(msg));
        return ParseStatus::Ok;
    }

    H* h_;
};

}  // namespace obe::feed
