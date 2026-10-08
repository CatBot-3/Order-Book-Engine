#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "obe/engine/concepts.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/codec.hpp"
#include "obe/net/epoll_transport.hpp"
#include "obe/net/protocol.hpp"
#include "obe/net/transport.hpp"
#include "obe/types.hpp"

// The order gateway: TCP in, matching engine in the middle, reports back out.
//
//   client ──TCP──> [ read, frame, decode ] ──> engine.submit / cancel / replace
//   client <──TCP── [ encode, queue, write ] <── the engine's reports
//
// One thread does all of it. A request is read, decoded and handed to the
// engine in the same call; the engine's reports are queued for the
// connections they belong to before that call returns; and everything queued
// is sent on its way at the end of the turn. Nothing is locked because
// nothing is shared.
//
// The gateway is the session and not the plumbing. Getting bytes in and out
// of sockets is a transport's job (transport.hpp), and the gateway is written
// once for any of them: epoll by default, io_uring through the third template
// parameter. What is here is what does not depend on that choice.
//
// A connection is an owner. The gateway gives each new connection the next
// owner id and never reuses one, so a report can always be routed, or
// recognised as addressed to somebody who has left.
//
// The three things a TCP server must get right, and where each is handled:
//
//   Partial reads. TCP delivers bytes, not messages. One read may hold half a
//   message, or three and a half. Each connection keeps the bytes of a message
//   that has not arrived completely, and consume() only acts on whole ones.
//
//   Slow clients. A client that stops reading must not stall the others or
//   grow the server's memory without bound. Sending never waits: what the
//   kernel will not take stays queued in the transport, and a client whose
//   backlog passes a limit is disconnected. The limit and the judgement are
//   here; the queue is the transport's.
//
//   Disconnects at any moment. A connection that closes in the middle of a
//   message just loses that message; the bytes are discarded with the
//   connection. Its resting orders are cancelled, so that nobody leaves orders
//   in the book that they can no longer manage.

namespace obe::net {

struct GatewayConfig {
    std::size_t max_connections = 4096;
    // Bytes queued for one client before it is treated as too slow.
    std::size_t max_pending_output = std::size_t{1} << 20;
    // Most bytes taken from one connection per readable event. A bound keeps
    // one flooding client from starving the rest of a turn.
    std::size_t read_chunk = std::size_t{64} << 10;
    bool cancel_on_disconnect = true;
};

struct GatewayStats {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_refused = 0;  // over max_connections
    std::uint64_t closed_by_peer = 0;       // the client closed, cleanly or not
    std::uint64_t closed_protocol = 0;      // a type byte that is not a request
    std::uint64_t closed_slow = 0;          // output backlog over the limit
    std::uint64_t closed_error = 0;         // any other socket error
    std::uint64_t requests = 0;             // complete messages decoded
    std::uint64_t bad_fields = 0;           // requests the gateway itself refused
    std::uint64_t responses = 0;            // messages queued to clients
    std::uint64_t responses_dropped = 0;    // addressed to a client that has left
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t blocked_writes = 0;  // times a socket would not take everything
    std::uint64_t orders_cancelled_on_disconnect = 0;

    friend bool operator==(const GatewayStats&, const GatewayStats&) = default;
};

// Impl names an engine (obe/engine/engines.hpp). MarketData is the engine's
// market-data sink, usually a MoldPacketizer. Net is how bytes reach the
// sockets (transport.hpp).
template <class Impl, engine::MarketDataSink MarketData, Transport Net = EpollTransport>
class OrderGateway {
 public:
    // The engine's report sink: turns each report into a message on the
    // owner's connection.
    class Router {
     public:
        explicit Router(OrderGateway& gateway) noexcept : gateway_(&gateway) {}

        // The list of an owner's open orders is kept whether or not the
        // report could be queued. A connection that is about to be dropped
        // (this very report may be the one that showed it to be too slow)
        // still has its orders cancelled when it is closed, and can only have
        // them cancelled if they were written down.
        void on_accepted(const engine::Accepted& r) {
            if (Connection* c = gateway_->deliver(r.owner, to_wire(r))) {
                c->open.insert(r.order_id);
            }
        }
        void on_executed(const engine::Executed& r) {
            Connection* c = gateway_->deliver(r.owner, to_wire(r));
            if (c != nullptr && r.leaves == 0) {
                c->open.erase(r.order_id);
            }
        }
        void on_cancelled(const engine::Cancelled& r) {
            if (Connection* c = gateway_->deliver(r.owner, to_wire(r))) {
                c->open.erase(r.order_id);
            }
        }
        void on_replaced(const engine::Replaced& r) {
            Connection* c = gateway_->deliver(r.owner, to_wire(r));
            if (c != nullptr && r.new_id != r.old_id) {
                c->open.erase(r.old_id);
                c->open.insert(r.new_id);
            }
        }
        void on_rejected(const engine::Rejected& r) {
            static_cast<void>(gateway_->deliver(r.owner, to_wire(r)));
        }

     private:
        OrderGateway* gateway_;
    };

    using Engine = typename Impl::template Engine<Router, MarketData>;
    using Clock = std::function<Nanos()>;

    // `clock` supplies the time stamped on everything the engine publishes.
    // The default counts nanoseconds from the gateway's creation; tests pass
    // their own to make a run repeatable.
    explicit OrderGateway(MarketData& market_data, const GatewayConfig& cfg = {}, Clock clock = {})
        : cfg_(cfg),
          clock_(clock ? std::move(clock) : since_now()),
          net_(TransportConfig{.read_chunk =
                                   std::max<std::size_t>(cfg.read_chunk, kMaxRequestSize)}),
          router_(*this),
          engine_(std::make_unique<Engine>(router_, market_data)) {}

    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;

    // Starts accepting connections. Port 0 lets the system choose; port()
    // says which it chose.
    void listen(const std::string& host, std::uint16_t port) { net_.listen(host, port); }

    [[nodiscard]] std::uint16_t port() const noexcept { return net_.port(); }

    // The engine, for opening instruments before trading starts.
    [[nodiscard]] Engine& engine() noexcept { return *engine_; }
    [[nodiscard]] const Engine& engine() const noexcept { return *engine_; }

    [[nodiscard]] Nanos now() const { return clock_(); }

    // One turn of the event loop: waits up to timeout_ms for something to
    // happen on a socket, serves everything that did, sends what that
    // produced on its way and closes the connections that ended. Returns the
    // number of things the transport served; 0 means the time ran out.
    std::size_t poll(int timeout_ms) {
        const std::size_t served = net_.poll(timeout_ms, *this);

        // A connection that is being dropped is sent nothing more: what was
        // queued for it this turn is discarded with it. So those go first.
        close_doomed();

        // Everything this turn queued goes out now.
        net_.flush_all(*this);

        // And the connections the flush found dead.
        close_doomed();
        return served;
    }

    // What the transport tells the gateway (transport.hpp). Public because a
    // transport calls them; nothing else should.
    ConnId on_connect() {
        if (connections_.size() >= cfg_.max_connections) {
            ++stats_.connections_refused;
            return 0;
        }
        const engine::OwnerId owner = ++last_owner_;
        connections_[owner].owner = owner;
        ++stats_.connections_accepted;
        return owner;
    }

    void on_data(ConnId id, std::span<const std::byte> bytes) {
        const auto it = connections_.find(id);
        if (it == connections_.end() || it->second.doomed != Close::No) {
            return;
        }
        Connection& c = it->second;
        if (c.in.empty()) {
            // The common case: nothing left over from last time. Serve the
            // requests straight out of the transport's buffer and keep only a
            // tail that stops in the middle of a message.
            const std::size_t used = consume(c, bytes);
            c.in.assign(bytes.begin() + static_cast<std::ptrdiff_t>(used), bytes.end());
        } else {
            c.in.insert(c.in.end(), bytes.begin(), bytes.end());
            const std::size_t used = consume(c, c.in);
            c.in.erase(c.in.begin(), c.in.begin() + static_cast<std::ptrdiff_t>(used));
        }
    }

    void on_disconnect(ConnId id, LinkState why) {
        const auto it = connections_.find(id);
        if (it != connections_.end()) {
            doom(it->second, why == LinkState::Failed ? Close::Error : Close::Peer);
        }
    }

    [[nodiscard]] std::size_t connections() const noexcept { return connections_.size(); }
    // The gateway's own counts, with the transport's byte counts folded in.
    [[nodiscard]] GatewayStats stats() const {
        GatewayStats s = stats_;
        const TransportStats t = net_.stats();
        s.bytes_in = t.bytes_in;
        s.bytes_out = t.bytes_out;
        s.blocked_writes = t.blocked_writes;
        return s;
    }

    // The transport, for its own statistics.
    [[nodiscard]] const Net& transport() const noexcept { return net_; }

 private:
    enum class Close : std::uint8_t { No, Peer, Protocol, Slow, Error };

    struct Connection {
        engine::OwnerId owner = 0;
        std::vector<std::byte> in;  // the start of a request still arriving
        std::unordered_set<OrderId> open;
        Close doomed = Close::No;
    };

    static Clock since_now() {
        const auto start = std::chrono::steady_clock::now();
        return [start] {
            return static_cast<Nanos>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
        };
    }

    void doom(Connection& c, Close why) {
        if (c.doomed == Close::No) {
            c.doomed = why;
            doomed_.push_back(c.owner);
        }
    }

    // Serves every complete request at the front of `data`. Returns how many
    // bytes it used; the rest is the start of a request that is still on its
    // way.
    std::size_t consume(Connection& c, std::span<const std::byte> data) {
        std::size_t at = 0;
        while (at < data.size() && c.doomed == Close::No) {
            const std::size_t size = request_size(static_cast<char>(data[at]));
            if (size == 0) {
                // Not a request type. There is no way to find the next
                // message boundary, so the stream is unusable from here.
                doom(c, Close::Protocol);
                break;
            }
            if (data.size() - at < size) {
                break;
            }
            handle(c, data.data() + at);
            ++stats_.requests;
            at += size;
        }
        return at;
    }

    void handle(Connection& c, const std::byte* message) {
        const Nanos time = clock_();
        switch (static_cast<char>(message[0])) {
            case EnterOrder::kType: {
                const auto m = feed::decode<EnterOrder>(message);
                engine::NewOrder order;
                if (to_engine(m, c.owner, order)) {
                    engine_->submit(order, time);
                } else {
                    ++stats_.bad_fields;
                    static_cast<void>(deliver(c.owner, OrderRejected{.timestamp = time,
                                                                     .token = m.token,
                                                                     .order_id = 0,
                                                                     .reason = kRejectBadField}));
                }
                break;
            }
            case ReplaceOrder::kType: {
                const auto m = feed::decode<ReplaceOrder>(message);
                engine_->replace(c.owner, m.order_id, m.qty, m.price, time);
                break;
            }
            case CancelOrder::kType: {
                const auto m = feed::decode<CancelOrder>(message);
                engine_->cancel(c.owner, m.order_id, time);
                break;
            }
            default:
                break;  // consume() only passes the three types above
        }
    }

    // Queues a message for an owner. Returns the owner's connection, for the
    // caller to keep its list of open orders by, or null if the owner has
    // gone. A connection that is about to be dropped is returned like any
    // other: nothing is queued for it, but it is still in the table and its
    // orders are still its own until it is closed.
    template <class M>
    Connection* deliver(engine::OwnerId owner, const M& message) {
        const auto it = connections_.find(owner);
        if (it == connections_.end()) {
            ++stats_.responses_dropped;
            return nullptr;
        }
        Connection& c = it->second;
        if (c.doomed != Close::No) {
            ++stats_.responses_dropped;
            return &c;
        }
        std::array<std::byte, kMaxResponseSize> bytes;
        feed::encode(message, bytes.data());
        net_.send(owner, {bytes.data(), feed::wire_size(message)});
        ++stats_.responses;
        if (net_.unsent(owner) > cfg_.max_pending_output) {
            // A large backlog is only a sign of a slow client if the socket
            // refuses it. A client that sent a big burst and is reading its
            // answers promptly has just as much queued at this point, so try
            // the socket before judging.
            const LinkState link = net_.flush(owner);
            if (link != LinkState::Up) {
                doom(c, link == LinkState::Failed ? Close::Error : Close::Peer);
            } else if (net_.unsent(owner) > cfg_.max_pending_output) {
                doom(c, Close::Slow);
            }
        }
        return &c;
    }

    // Closing cancels orders, which publishes market data and can only queue
    // reports for the connection being closed, which is already gone by then.
    // So this loop does not grow doomed_. It is by index all the same, so
    // that it would stay correct if some later change made a close doom
    // another connection: an iterator would not survive the vector growing.
    void close_doomed() {
        // NOLINTNEXTLINE(modernize-loop-convert)
        for (std::size_t i = 0; i < doomed_.size(); ++i) {
            close_connection(doomed_[i]);
        }
        doomed_.clear();
    }

    void close_connection(engine::OwnerId owner) {
        // Out of the table first: from here on, reports for this owner are
        // dropped instead of queued.
        auto node = connections_.extract(owner);
        if (node.empty()) {
            return;
        }
        Connection& c = node.mapped();
        switch (c.doomed) {
            case Close::Protocol:
                ++stats_.closed_protocol;
                break;
            case Close::Slow:
                ++stats_.closed_slow;
                break;
            case Close::Error:
                ++stats_.closed_error;
                break;
            case Close::Peer:
            case Close::No:
                ++stats_.closed_by_peer;
                break;
        }
        net_.close(owner);
        if (cfg_.cancel_on_disconnect && !c.open.empty()) {
            // In id order, so that the market data this publishes does not
            // depend on how a hash set happens to iterate.
            std::vector<OrderId> ids(c.open.begin(), c.open.end());
            std::sort(ids.begin(), ids.end());
            const Nanos time = clock_();
            for (const OrderId id : ids) {
                if (engine_->cancel(owner, id, time)) {
                    ++stats_.orders_cancelled_on_disconnect;
                }
            }
        }
    }

    GatewayConfig cfg_;
    Clock clock_;
    Net net_;
    Router router_;
    std::unique_ptr<Engine> engine_;
    std::unordered_map<engine::OwnerId, Connection> connections_;
    std::vector<engine::OwnerId> doomed_;  // connections to close at the end of the turn
    engine::OwnerId last_owner_ = 0;
    GatewayStats stats_{};  // bytes and blocked writes are the transport's: see stats()
};

}  // namespace obe::net
