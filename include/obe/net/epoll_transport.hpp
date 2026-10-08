#pragma once

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "obe/net/epoll_loop.hpp"
#include "obe/net/out_buffer.hpp"
#include "obe/net/socket.hpp"
#include "obe/net/transport.hpp"

// The readiness transport: epoll says which sockets can be read or written
// without waiting, and one recv() or send() per socket then does it.
//
// The contract is in transport.hpp. This is the reference for it: every
// buffer is the program's own the whole time, every operation happens inside
// the call that asks for it, and so there is very little that can be got
// wrong. What it costs is system calls: a turn with N busy connections is
// one epoll_wait, N recv and up to N send.
//
// Two things in it are choices and not necessities.
//
//   Writes are tried first and waited for second. send() is called as soon
//   as there is something to write, on the bet that the socket has room,
//   which it nearly always does. Only when the kernel refuses is the socket
//   watched for writability (EPOLLOUT), and the watch is dropped again as
//   soon as the backlog is gone. Watching every socket for writability all
//   the time would wake the loop on every turn for nothing: an idle socket
//   is always writable.
//
//   One read per readable event, of at most read_chunk bytes. Level-triggered
//   epoll reports the socket again next turn if more is waiting, so nothing
//   is lost by stopping, and one flooding client cannot hold a turn.

namespace obe::net {

class EpollTransport {
 public:
    static constexpr std::string_view kName = "epoll";
    static constexpr std::string_view kDescription =
        "readiness: epoll says which sockets are ready, then one recv or send each";

    [[nodiscard]] static bool available() noexcept { return true; }

    explicit EpollTransport(const TransportConfig& cfg = {})
        : scratch_(std::max<std::size_t>(cfg.read_chunk, 1)) {}

    EpollTransport(const EpollTransport&) = delete;
    EpollTransport& operator=(const EpollTransport&) = delete;

    void listen(const std::string& host, std::uint16_t port) {
        listener_ = listen_tcp(host, port);
        port_ = local_port(listener_.get());
        loop_.add(listener_.get(), EPOLLIN, kListenerTag);
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    template <TransportHandler H>
    std::size_t poll(int timeout_ms, H& handler) {
        ++stats_.syscalls;
        const std::span<const epoll_event> events = loop_.wait(timeout_ms);
        for (const epoll_event& ev : events) {
            if (ev.data.u64 == kListenerTag) {
                accept_all(handler);
                continue;
            }
            const auto it = connections_.find(static_cast<ConnId>(ev.data.u64));
            if (it == connections_.end()) {
                continue;
            }
            // Callbacks cannot close a connection or accept one, so this
            // reference outlives them.
            Connection& c = it->second;
            if ((ev.events & EPOLLIN) != 0 && c.state == LinkState::Up) {
                read_from(c, handler);
            }
            if ((ev.events & EPOLLOUT) != 0 && c.state == LinkState::Up) {
                write_out(c);
            }
            if ((ev.events & (EPOLLERR | EPOLLHUP)) != 0 && c.state == LinkState::Up) {
                c.state = LinkState::ClosedByPeer;
            }
            report_if_dead(c, handler);
        }
        return events.size();
    }

    void send(ConnId id, std::span<const std::byte> bytes) {
        const auto it = connections_.find(id);
        if (it == connections_.end() || it->second.state != LinkState::Up) {
            return;
        }
        Connection& c = it->second;
        c.out.append(bytes);
        if (!c.dirty) {
            c.dirty = true;
            dirty_.push_back(id);
        }
    }

    template <TransportHandler H>
    void flush_all(H& handler) {
        // By index: on_disconnect may send to other connections, which can
        // add to dirty_ while it is being walked.
        // NOLINTNEXTLINE(modernize-loop-convert)
        for (std::size_t i = 0; i < dirty_.size(); ++i) {
            const auto it = connections_.find(dirty_[i]);
            if (it == connections_.end()) {
                continue;
            }
            Connection& c = it->second;
            c.dirty = false;
            if (c.state == LinkState::Up) {
                write_out(c);
            }
            report_if_dead(c, handler);
        }
        dirty_.clear();
    }

    LinkState flush(ConnId id) {
        const auto it = connections_.find(id);
        if (it == connections_.end()) {
            return LinkState::Failed;
        }
        Connection& c = it->second;
        if (c.state == LinkState::Up) {
            write_out(c);
        }
        if (c.state != LinkState::Up && !c.reported) {
            retire(c);  // this return value is the report
        }
        return c.state;
    }

    [[nodiscard]] std::size_t unsent(ConnId id) const {
        const auto it = connections_.find(id);
        return it == connections_.end() ? 0 : it->second.out.unsent();
    }

    void close(ConnId id) {
        const auto it = connections_.find(id);
        if (it == connections_.end()) {
            return;
        }
        loop_.remove(it->second.fd.get());
        connections_.erase(it);  // closes the socket
        stats_.syscalls += 2;    // epoll_ctl, close
    }

    [[nodiscard]] const TransportStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t connections() const noexcept { return connections_.size(); }

 private:
    static constexpr std::uint64_t kListenerTag = 0;  // a ConnId is never 0

    struct Connection {
        Fd fd;
        ConnId id = 0;
        OutBuffer out;  // queued and not yet taken by the kernel
        LinkState state = LinkState::Up;
        bool reported = false;     // the handler has been told it is dead
        bool dirty = false;        // in dirty_
        bool wants_write = false;  // watched for EPOLLOUT
    };

    template <class H>
    void accept_all(H& handler) {
        for (;;) {
            ++stats_.syscalls;
            Fd fd(::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!fd.valid()) {
                return;  // nobody else waiting (or a transient error; try next turn)
            }
            const ConnId id = handler.on_connect();
            if (id == 0) {
                ++stats_.syscalls;  // the close as fd goes out of scope
                continue;
            }
            const int one = 1;
            ::setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            const int raw = fd.get();
            Connection& c = connections_[id];
            c = Connection{};
            c.fd = std::move(fd);
            c.id = id;
            loop_.add(raw, EPOLLIN, id);
            stats_.syscalls += 2;  // setsockopt, epoll_ctl
        }
    }

    template <class H>
    void read_from(Connection& c, H& handler) {
        ++stats_.syscalls;
        const ssize_t n = ::recv(c.fd.get(), scratch_.data(), scratch_.size(), 0);
        if (n == 0) {
            c.state = LinkState::ClosedByPeer;
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                c.state = errno == ECONNRESET ? LinkState::ClosedByPeer : LinkState::Failed;
            }
            return;
        }
        const auto got = static_cast<std::size_t>(n);
        stats_.bytes_in += got;
        handler.on_data(c.id, std::span<const std::byte>(scratch_.data(), got));
    }

    // Writes as much of the connection's output as the socket will take.
    void write_out(Connection& c) {
        while (!c.out.empty()) {
            ++stats_.syscalls;
            const std::span<const std::byte> next = c.out.pending();
            const ssize_t n = ::send(c.fd.get(), next.data(), next.size(), MSG_NOSIGNAL);
            if (n > 0) {
                c.out.took(static_cast<std::size_t>(n));
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
            c.state =
                errno == EPIPE || errno == ECONNRESET ? LinkState::ClosedByPeer : LinkState::Failed;
            return;
        }
        watch_writable(c, false);
    }

    void watch_writable(Connection& c, bool on) {
        if (c.wants_write != on) {
            c.wants_write = on;
            ++stats_.syscalls;
            loop_.modify(c.fd.get(), on ? (EPOLLIN | EPOLLOUT) : EPOLLIN, c.id);
        }
    }

    template <class H>
    void report_if_dead(Connection& c, H& handler) {
        if (c.state != LinkState::Up && !c.reported) {
            retire(c);
            handler.on_disconnect(c.id, c.state);
        }
    }

    // A connection found dead is told about once, by whoever found it, and
    // is no longer watched: nothing more is read or written, and a socket
    // whose peer has gone stays "ready" for ever, so it would otherwise wake
    // every turn until the handler closes it.
    void retire(Connection& c) {
        c.reported = true;
        loop_.remove(c.fd.get());
        ++stats_.syscalls;
    }

    EpollLoop loop_;
    Fd listener_;
    std::uint16_t port_ = 0;
    std::unordered_map<ConnId, Connection> connections_;
    std::vector<ConnId> dirty_;       // connections with output queued since the last flush_all
    std::vector<std::byte> scratch_;  // where a read lands
    TransportStats stats_{};
};

static_assert(Transport<EpollTransport>);

}  // namespace obe::net
