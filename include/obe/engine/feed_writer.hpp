#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "obe/engine/concepts.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/feed/codec.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"

// Market-data sinks for the engine.
//
// ItchFeedWriter turns the engine's market data into bytes in the layout of
// Nasdaq's sample files: a two-byte big-endian length, then the message. What
// it produces can be written to a file and read back by every tool in this
// project that reads a real day (itch_stats, book_replay, replay_bench).
//
// That closes the loop the spec calls the round trip (success criterion 7):
// engine -> ItchFeedWriter -> bytes -> ItchParser -> BookManager must rebuild
// the engine's own book. The two halves were written separately and share
// only the message definitions, so each checks the other.

namespace obe::engine {

class ItchFeedWriter {
 public:
    // Messages by type byte, for summaries and tests.
    using Counts = std::array<std::uint64_t, 256>;

    void on_system_event(const feed::SystemEvent& m) { append(m); }
    void on_stock_directory(const feed::StockDirectory& m) { append(m); }
    void on_trading_action(const feed::TradingAction& m) { append(m); }
    void on_add(const feed::AddOrder& m) { append(m); }
    void on_execute(const feed::OrderExecuted& m) { append(m); }
    void on_cancel(const feed::OrderCancel& m) { append(m); }
    void on_delete(const feed::OrderDelete& m) { append(m); }
    void on_replace(const feed::OrderReplace& m) { append(m); }

    // The engine does not publish System Events: it has no notion of a
    // trading day. Whoever runs it brackets the stream with these.
    void system_event(char code, Nanos now) {
        append(feed::SystemEvent{.hdr = md::header(0, now), .event_code = code});
    }

    // The bytes written since construction or the last take().
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return out_; }

    // Hands the buffered bytes over and starts an empty buffer. Counts keep
    // running. A long run calls this now and then to write the buffer to disk
    // instead of holding the whole stream in memory.
    [[nodiscard]] std::vector<std::byte> take() { return std::exchange(out_, {}); }

    [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }
    [[nodiscard]] std::uint64_t total_bytes() const noexcept { return total_bytes_; }
    [[nodiscard]] std::uint64_t count(char type) const noexcept {
        return counts_[static_cast<unsigned char>(type)];
    }

 private:
    template <class M>
    void append(const M& m) {
        const std::size_t before = out_.size();
        feed::append_framed(out_, m);
        total_bytes_ += out_.size() - before;
        ++messages_;
        ++counts_[static_cast<unsigned char>(m.type())];
    }

    std::vector<std::byte> out_;
    Counts counts_{};
    std::uint64_t messages_ = 0;
    std::uint64_t total_bytes_ = 0;
};

static_assert(MarketDataSink<ItchFeedWriter>);

// Sends the market data to two sinks, `first` before `second`. Used to feed a
// book directly while also writing the bytes.
template <MarketDataSink A, MarketDataSink B>
class TeeMarketData {
 public:
    TeeMarketData(A& first, B& second) : a_(&first), b_(&second) {}

    void on_stock_directory(const feed::StockDirectory& m) {
        a_->on_stock_directory(m);
        b_->on_stock_directory(m);
    }
    void on_trading_action(const feed::TradingAction& m) {
        a_->on_trading_action(m);
        b_->on_trading_action(m);
    }
    void on_add(const feed::AddOrder& m) {
        a_->on_add(m);
        b_->on_add(m);
    }
    void on_execute(const feed::OrderExecuted& m) {
        a_->on_execute(m);
        b_->on_execute(m);
    }
    void on_cancel(const feed::OrderCancel& m) {
        a_->on_cancel(m);
        b_->on_cancel(m);
    }
    void on_delete(const feed::OrderDelete& m) {
        a_->on_delete(m);
        b_->on_delete(m);
    }
    void on_replace(const feed::OrderReplace& m) {
        a_->on_replace(m);
        b_->on_replace(m);
    }

 private:
    A* a_;
    B* b_;
};

// The same for reports.
template <ReportSink A, ReportSink B>
class TeeReports {
 public:
    TeeReports(A& first, B& second) : a_(&first), b_(&second) {}

    void on_accepted(const Accepted& r) {
        a_->on_accepted(r);
        b_->on_accepted(r);
    }
    void on_executed(const Executed& r) {
        a_->on_executed(r);
        b_->on_executed(r);
    }
    void on_cancelled(const Cancelled& r) {
        a_->on_cancelled(r);
        b_->on_cancelled(r);
    }
    void on_replaced(const Replaced& r) {
        a_->on_replaced(r);
        b_->on_replaced(r);
    }
    void on_rejected(const Rejected& r) {
        a_->on_rejected(r);
        b_->on_rejected(r);
    }

 private:
    A* a_;
    B* b_;
};

}  // namespace obe::engine
