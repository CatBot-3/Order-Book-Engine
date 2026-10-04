#pragma once

#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
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
#include "obe/net/epoll_loop.hpp"
#include "obe/net/protocol.hpp"
#include "obe/net/socket.hpp"
#include "obe/types.hpp"

// The order gateway: TCP in, matching engine in the middle, reports back out.
//
//   client ──TCP──> [ read, frame, decode ] ──> engine.submit / cancel / replace
//   client <──TCP── [ encode, queue, write ] <── the engine's reports
//
// One thread does all of it, driven by epoll. A request is read, decoded and
// handed to the engine in the same call; the engine's reports land in the
// output buffers of the connections they belong to before that call returns;
// and every buffer that gained something is written out at the end of the
// turn. Nothing is locked because nothing is shared.
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
//   grow the server's memory without bound. Writes never block: what the
//   socket will not take is kept, the connection is watched for writability,
//   and a client whose backlog passes a limit is disconnected.
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
// market-data sink, usually a MoldPacketizer.
template <class Impl, engine::MarketDataSink MarketData>
class OrderGateway {
 public:
    // The engine's report sink: turns each report into a message on the
    // owner's connection.
    class Router {
     public:
        explicit Router(OrderGateway& gateway) noexcept : gateway_(&gateway) {}

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
          router_(*this),
          engine_(std::make_unique<Engine>(router_, market_data)),
          scratch_(std::max<std::size_t>(cfg.read_chunk, kMaxRequestSize)) {}

    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;

    // Starts accepting connections. Port 0 lets the system choose; port()
    // says which it chose.
    void listen(const std::string& host, std::uint16_t port) {
        listener_ = listen_tcp(host, port);
        port_ = local_port(listener_.get());
        loop_.add(listener_.get(), EPOLLIN, kListenerTag);
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    // The engine, for opening instruments before trading starts.
    [[nodiscard]] Engine& engine() noexcept { return *engine_; }
    [[nodiscard]] const Engine& engine() const noexcept { return *engine_; }

    [[nodiscard]] Nanos now() const { return clock_(); }

    // One turn of the event loop: waits up to timeout_ms for sockets to become
    // ready, serves every one that is, writes out what that produced and
    // closes the connections that ended. Returns the number of ready sockets.
    std::size_t poll(int timeout_ms) {
        const std::span<const epoll_event> events = loop_.wait(timeout_ms);
        for (const epoll_event& ev : events) {
            if (ev.data.u64 == kListenerTag) {
                accept_all();
                continue;
            }
            const auto it = connections_.find(static_cast<engine::OwnerId>(ev.data.u64));
            if (it == connections_.end()) {
                continue;
            }
            Connection& c = it->second;
            if ((ev.events & EPOLLIN) != 0 && c.doomed == Close::No) {
                read_from(c);
            }
            if ((ev.events & EPOLLOUT) != 0 && c.doomed == Close::No) {
                flush(c);
            }
            if ((ev.events & (EPOLLERR | EPOLLHUP)) != 0) {
                doom(c, Close::Peer);
            }
        }

        // Everything this turn queued goes out now, one write per connection.
        // Flushing can doom a connection but does not add to dirty_. The loop
        // is by index all the same, so that it would stay correct if some
        // later change made a flush queue output: an iterator would not
        // survive the vector growing.
        // NOLINTNEXTLINE(modernize-loop-convert)
        for (std::size_t i = 0; i < dirty_.size(); ++i) {
            const auto it = connections_.find(dirty_[i]);
            if (it != connections_.end()) {
                it->second.dirty = false;
                if (it->second.doomed == Close::No) {
                    flush(it->second);
                }
            }
        }
        dirty_.clear();

        // Closing cancels orders, which publishes market data and can only
        // queue reports for the connection being closed, which is already
        // gone by then. So this loop does not grow doomed_ or dirty_ either,
        // and is by index for the same reason as the one above.
        // NOLINTNEXTLINE(modernize-loop-convert)
        for (std::size_t i = 0; i < doomed_.size(); ++i) {
            close_connection(doomed_[i]);
        }
        doomed_.clear();
        return events.size();
    }

    [[nodiscard]] std::size_t connections() const noexcept { return connections_.size(); }
    [[nodiscard]] const GatewayStats& stats() const noexcept { return stats_; }

 private:
    static constexpr std::uint64_t kListenerTag = 0;  // owner ids start at 1

    enum class Close : std::uint8_t { No, Peer, Protocol, Slow, Error };

    struct Connection {
        Fd fd;
        engine::OwnerId owner = 0;
        std::vector<std::byte> in;   // the start of a request still arriving
        std::vector<std::byte> out;  // responses not yet written
        std::size_t out_sent = 0;    // how much of `out` has been written
        std::unordered_set<OrderId> open;
        Close doomed = Close::No;
        bool dirty = false;        // in dirty_ this turn
        bool wants_write = false;  // watching for EPOLLOUT
    };

    static Clock since_now() {
        const auto start = std::chrono::steady_clock::now();
        return [start] {
            return static_cast<Nanos>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
        };
    }

    void accept_all() {
        for (;;) {
            Fd fd(::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!fd.valid()) {
                return;  // nobody else waiting (or a transient error; try next turn)
            }
            if (connections_.size() >= cfg_.max_connections) {
                ++stats_.connections_refused;
                continue;  // fd closes here
            }
            const int one = 1;
            ::setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            const engine::OwnerId owner = ++last_owner_;
            const int raw = fd.get();
            Connection& c = connections_[owner];
            c.fd = std::move(fd);
            c.owner = owner;
            loop_.add(raw, EPOLLIN, owner);
            ++stats_.connections_accepted;
        }
    }

    void doom(Connection& c, Close why) {
        if (c.doomed == Close::No) {
            c.doomed = why;
            doomed_.push_back(c.owner);
        }
    }

    void read_from(Connection& c) {
        const ssize_t n = ::recv(c.fd.get(), scratch_.data(), scratch_.size(), 0);
        if (n == 0) {
            doom(c, Close::Peer);
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                doom(c, errno == ECONNRESET ? Close::Peer : Close::Error);
            }
            return;
        }
        const auto got = static_cast<std::size_t>(n);
        stats_.bytes_in += got;
        if (c.in.empty()) {
            // The common case: nothing left over from last time. Serve the
            // requests straight out of the read buffer and keep only a tail
            // that stops in the middle of a message.
            const std::size_t used = consume(c, {scratch_.data(), got});
            c.in.assign(scratch_.begin() + static_cast<std::ptrdiff_t>(used),
                        scratch_.begin() + static_cast<std::ptrdiff_t>(got));
        } else {
            c.in.insert(c.in.end(), scratch_.begin(),
                        scratch_.begin() + static_cast<std::ptrdiff_t>(got));
            const std::size_t used = consume(c, c.in);
            c.in.erase(c.in.begin(), c.in.begin() + static_cast<std::ptrdiff_t>(used));
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

    // Queues a message for an owner. Returns the owner's connection, or null
    // if the owner has gone or is about to be dropped.
    template <class M>
    Connection* deliver(engine::OwnerId owner, const M& message) {
        const auto it = connections_.find(owner);
        if (it == connections_.end() || it->second.doomed != Close::No) {
            ++stats_.responses_dropped;
            return nullptr;
        }
        Connection& c = it->second;
        const std::size_t at = c.out.size();
        c.out.resize(at + feed::wire_size(message));
        feed::encode(message, c.out.data() + at);
        ++stats_.responses;
        if (!c.dirty) {
            c.dirty = true;
            dirty_.push_back(owner);
        }
        if (c.out.size() - c.out_sent > cfg_.max_pending_output) {
            // A large backlog is only a sign of a slow client if the socket
            // refuses it. A client that sent a big burst and is reading its
            // answers promptly has just as much queued at this point, so try
            // the socket before judging.
            flush(c);
            if (c.doomed == Close::No && c.out.size() - c.out_sent > cfg_.max_pending_output) {
                doom(c, Close::Slow);
            }
            if (c.doomed != Close::No) {
                return nullptr;
            }
        }
        return &c;
    }

    // Writes as much of the connection's output as the socket will take.
    void flush(Connection& c) {
        while (c.out_sent < c.out.size()) {
            const ssize_t n = ::send(c.fd.get(), c.out.data() + c.out_sent,
                                     c.out.size() - c.out_sent, MSG_NOSIGNAL);
            if (n > 0) {
                c.out_sent += static_cast<std::size_t>(n);
                stats_.bytes_out += static_cast<std::uint64_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // The socket's buffer is full: the client is not reading as
                // fast as it is being written to. Keep the rest and ask to be
                // told when there is room.
                ++stats_.blocked_writes;
                watch_writable(c, true);
                return;
            }
            doom(c, errno == EPIPE || errno == ECONNRESET ? Close::Peer : Close::Error);
            return;
        }
        c.out.clear();
        c.out_sent = 0;
        watch_writable(c, false);
    }

    void watch_writable(Connection& c, bool on) {
        if (c.wants_write != on) {
            c.wants_write = on;
            loop_.modify(c.fd.get(), on ? (EPOLLIN | EPOLLOUT) : EPOLLIN, c.owner);
        }
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
        loop_.remove(c.fd.get());
        c.fd.reset();
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
    EpollLoop loop_;
    Fd listener_;
    std::uint16_t port_ = 0;
    Router router_;
    std::unique_ptr<Engine> engine_;
    std::unordered_map<engine::OwnerId, Connection> connections_;
    std::vector<engine::OwnerId> dirty_;   // connections with output queued this turn
    std::vector<engine::OwnerId> doomed_;  // connections to close at the end of the turn
    std::vector<std::byte> scratch_;       // where a read lands
    engine::OwnerId last_owner_ = 0;
    GatewayStats stats_{};
};

}  // namespace obe::net
