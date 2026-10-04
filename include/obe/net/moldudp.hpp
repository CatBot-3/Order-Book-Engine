#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "obe/engine/concepts.hpp"
#include "obe/feed/codec.hpp"
#include "obe/feed/endian.hpp"
#include "obe/feed/handler.hpp"
#include "obe/feed/messages.hpp"
#include "obe/feed/parser.hpp"

// Market data over UDP, framed in the style of Nasdaq's MoldUDP64.
//
// UDP gives no guarantees: a datagram can be lost, duplicated or arrive out
// of order, and the receiver is told nothing. Exchanges use it anyway, because
// one multicast send reaches every subscriber at once and no slow subscriber
// can hold the others up. The framing is what makes the losses visible.
//
//   packet:  session   10 bytes, ASCII, names the stream
//            sequence   8 bytes, big-endian: the number of the first message
//            count      2 bytes, big-endian: messages in this packet
//            then `count` blocks of:
//                length  2 bytes, big-endian
//                message `length` bytes (an ITCH message, type byte first)
//
// Every message has a sequence number, counted from 1 and never reused. A
// packet carries the number of its first message; the rest follow. The
// receiver keeps the number it expects next:
//
//   packet starts above it   messages were lost: a gap
//   packet starts below it   a duplicate or a late packet: skip what was seen
//
// A packet with count 0 is a heartbeat: it carries the next sequence number,
// so a receiver learns of a loss at the tail of a burst without waiting for
// the next message. Count 0xFFFF marks the end of the session.
//
// What is not here: recovery. Real MoldUDP64 has a second port that
// re-sends a range of messages on request. This receiver detects and counts a
// gap, and its book is wrong from then on; it says so and carries on.
//
// The packet logic lives here with no sockets in it, so it can be tested byte
// for byte. obe/net/udp.hpp puts sockets around it.

namespace obe::net {

inline constexpr std::size_t kMoldHeaderSize = 20;
inline constexpr std::size_t kMoldSessionSize = 10;
inline constexpr std::uint16_t kMoldEndOfSession = 0xFFFF;
// Leaves room for IP and UDP headers inside a 1500-byte Ethernet frame, with
// some to spare for tunnels. A datagram that does not fit in one frame is
// fragmented, and losing any fragment loses all of it.
inline constexpr std::size_t kMoldDefaultPayload = 1400;

using MoldSession = std::array<char, kMoldSessionSize>;

// A session name, left-justified and padded with spaces.
[[nodiscard]] constexpr MoldSession make_session(std::string_view name) noexcept {
    MoldSession out{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    for (std::size_t i = 0; i < out.size() && i < name.size(); ++i) {
        out[i] = name[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

// A market-data sink (engine::MarketDataSink) that packs messages into
// packets and hands each finished packet to `send`.
//
// A packet goes out when the next message would not fit, and when flush() is
// called. The gateway flushes once per turn of its event loop, so everything
// one burst of orders produced travels together and nothing waits for the
// next burst.
template <class Send>
class MoldPacketizer {
 public:
    MoldPacketizer(const MoldSession& session, Send send,
                   std::size_t max_payload = kMoldDefaultPayload)
        : send_(std::move(send)),
          session_(session),
          max_payload_(max_payload < kMinPayload ? kMinPayload : max_payload) {
        packet_.reserve(max_payload_);
        start_packet();
    }

    void on_system_event(const feed::SystemEvent& m) { append(m); }
    void on_stock_directory(const feed::StockDirectory& m) { append(m); }
    void on_trading_action(const feed::TradingAction& m) { append(m); }
    void on_add(const feed::AddOrder& m) { append(m); }
    void on_execute(const feed::OrderExecuted& m) { append(m); }
    void on_cancel(const feed::OrderCancel& m) { append(m); }
    void on_delete(const feed::OrderDelete& m) { append(m); }
    void on_replace(const feed::OrderReplace& m) { append(m); }

    // Sends the packet being built, if it holds any message.
    void flush() {
        if (count_ == 0) {
            return;
        }
        feed::store_be<std::uint16_t>(packet_.data() + kCountOffset, count_);
        send_(std::span<const std::byte>(packet_));
        ++packets_;
        next_sequence_ += count_;
        start_packet();
    }

    // Sends a packet with no messages that states the next sequence number.
    // Any pending messages go out first.
    void heartbeat() {
        flush();
        send_(std::span<const std::byte>(packet_));  // a header with count 0
        ++packets_;
    }

    // Tells receivers the stream is over. Any pending messages go out first.
    void end_of_session() {
        flush();
        feed::store_be<std::uint16_t>(packet_.data() + kCountOffset, kMoldEndOfSession);
        send_(std::span<const std::byte>(packet_));
        ++packets_;
        start_packet();
    }

    // The sequence number the next message will get.
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_ + count_; }
    [[nodiscard]] std::uint64_t packets() const noexcept { return packets_; }
    [[nodiscard]] std::uint64_t messages() const noexcept { return next_sequence() - 1; }

 private:
    static constexpr std::size_t kSequenceOffset = kMoldSessionSize;
    static constexpr std::size_t kCountOffset = kMoldSessionSize + 8;
    // Room for the header and the largest message, whatever the caller asked.
    static constexpr std::size_t kMinPayload = kMoldHeaderSize + 2 + feed::kMaxMessageSize;

    void start_packet() {
        packet_.resize(kMoldHeaderSize);
        std::memcpy(packet_.data(), session_.data(), session_.size());
        feed::store_be<std::uint64_t>(packet_.data() + kSequenceOffset, next_sequence_);
        feed::store_be<std::uint16_t>(packet_.data() + kCountOffset, 0);
        count_ = 0;
    }

    template <class M>
    void append(const M& m) {
        const std::size_t size = feed::wire_size(m);
        if (packet_.size() + 2 + size > max_payload_ || count_ == kMoldEndOfSession - 1) {
            flush();
        }
        const std::size_t at = packet_.size();
        packet_.resize(at + 2 + size);
        feed::store_be<std::uint16_t>(packet_.data() + at, static_cast<std::uint16_t>(size));
        feed::encode(m, packet_.data() + at + 2);
        ++count_;
    }

    Send send_;
    MoldSession session_;
    std::size_t max_payload_;
    std::vector<std::byte> packet_;
    std::uint64_t next_sequence_ = 1;  // of the first message in packet_
    std::uint16_t count_ = 0;          // messages in packet_
    std::uint64_t packets_ = 0;
};

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

struct MoldStats {
    std::uint64_t packets = 0;          // well-formed packets of this session
    std::uint64_t messages = 0;         // messages delivered to the handler
    std::uint64_t heartbeats = 0;       // packets with count 0
    std::uint64_t gaps = 0;             // times the sequence jumped forward
    std::uint64_t missed_messages = 0;  // messages inside those jumps
    std::uint64_t duplicates = 0;       // messages skipped because already seen
    std::uint64_t bad_packets = 0;      // too short, or a block ran past the end
    std::uint64_t bad_messages = 0;     // a block that is not a valid ITCH message
    std::uint64_t other_sessions = 0;   // packets of a different session
    bool ended = false;                 // the end-of-session packet was seen

    friend bool operator==(const MoldStats&, const MoldStats&) = default;
};

// Checks the sequence of incoming packets and hands the messages, each at
// most once and in order, to an ITCH handler.
template <feed::ItchHandler Handler>
class MoldReceiver {
 public:
    MoldReceiver(const MoldSession& session, Handler& handler)
        : session_(session), parser_(handler) {}

    // One datagram. Returns false if it was not a well-formed packet of this
    // session; the statistics say why.
    bool on_packet(std::span<const std::byte> packet) {
        if (packet.size() < kMoldHeaderSize) {
            ++stats_.bad_packets;
            return false;
        }
        if (std::memcmp(packet.data(), session_.data(), session_.size()) != 0) {
            ++stats_.other_sessions;
            return false;
        }
        const auto first = feed::load_be<std::uint64_t>(packet.data() + kMoldSessionSize);
        const auto count = feed::load_be<std::uint16_t>(packet.data() + kMoldSessionSize + 8);
        ++stats_.packets;

        if (count == kMoldEndOfSession) {
            note_sequence(first);
            stats_.ended = true;
            return true;
        }
        if (count == 0) {
            ++stats_.heartbeats;
            note_sequence(first);
            return true;
        }

        std::size_t at = kMoldHeaderSize;
        for (std::uint16_t i = 0; i < count; ++i) {
            if (packet.size() - at < 2) {
                ++stats_.bad_packets;
                return false;
            }
            const std::size_t length = feed::load_be<std::uint16_t>(packet.data() + at);
            at += 2;
            if (packet.size() - at < length) {
                ++stats_.bad_packets;
                return false;
            }
            const std::uint64_t sequence = first + i;
            if (sequence < expected_) {
                ++stats_.duplicates;  // seen before: a repeated or late packet
            } else {
                note_sequence(sequence);
                if (parser_.dispatch(packet.data() + at, length) == feed::ParseStatus::Ok) {
                    ++stats_.messages;
                } else {
                    ++stats_.bad_messages;
                }
                expected_ = sequence + 1;
            }
            at += length;
        }
        return true;
    }

    // The sequence number of the next message this receiver has not seen.
    [[nodiscard]] std::uint64_t expected() const noexcept { return expected_; }
    [[nodiscard]] const MoldStats& stats() const noexcept { return stats_; }

    // True if no message has been missed so far: what the handler was given is
    // the whole stream up to expected().
    [[nodiscard]] bool complete() const noexcept { return stats_.missed_messages == 0; }

 private:
    // The sender says its next message is `sequence`. If that is beyond what
    // was expected, the messages in between are gone.
    void note_sequence(std::uint64_t sequence) noexcept {
        if (sequence > expected_) {
            ++stats_.gaps;
            stats_.missed_messages += sequence - expected_;
            expected_ = sequence;
        }
    }

    MoldSession session_;
    feed::ItchParser<Handler> parser_;
    std::uint64_t expected_ = 1;
    MoldStats stats_{};
};

}  // namespace obe::net
