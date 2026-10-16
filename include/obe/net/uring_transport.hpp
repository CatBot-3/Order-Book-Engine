#pragma once

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "obe/net/socket.hpp"
#include "obe/net/transport.hpp"
#include "obe/net/uring.hpp"

// UringTransport: the completion transport, on io_uring.
//
// EpollTransport asks the kernel which sockets are ready and then reads and
// writes them itself, one system call each. This one tells the kernel in
// advance what to do ("receive from this socket into this buffer", "send
// these bytes") and collects the results afterwards. A whole turn of the
// event loop then costs two system calls, however many clients were busy.
//
// The contract is transport.hpp; the rings are uring.hpp, and its comment on
// "the rule that makes completion-based I/O hard" is the background to
// nearly every design point below. The judges are the tests the epoll
// transport passes, built a second time:
//
//   tests/net/transport_test.cpp   the contract, over real loopback sockets
//   tests/net/gateway_test.cpp     the whole gateway on top of it
//
//   ctest --preset debug -L hand-written -R 'Transport|Gateway|quarantine'
//
// The third name is the same tests once more with AddressSanitizer told to
// hand freed memory straight back (tests/CMakeLists.txt says why): the run in
// which a buffer freed or moved while the kernel still uses it has the best
// chance of showing. And the measurement:
//
//   build/release/bench/transport_bench --cpus 2,4
//   build/release/apps/exchange_server --io uring ...   with load_gen, as in
//                                                       the README
//
// DESIGN
//
//  1. user_data: the connection's address, with the operation in its low bit.
//     A connection object is allocated on its own (std::unique_ptr) and never
//     moves, and it is aligned to 8, so its address has three zero bits at
//     the bottom: the lowest says "receive" or "send". A completion then
//     leads straight to its connection with no lookup. A late completion for
//     a closed connection cannot be taken for the new connection that was
//     given the same id, because it names the old OBJECT, not the id, and
//     the old object stays alive (point 8) until its last completion has
//     arrived, so its address is not reused while one may still come. An
//     index and a generation would do the same with a table lookup and a
//     compare; the id alone would be wrong after reuse.
//
//  2. Receives land in a buffer per connection.
//     The buffer must be named when the receive is asked for, before
//     anything has arrived, and it is the kernel's until the completion, so
//     every connection with a receive waiting needs its own: read_chunk bytes
//     each. For 4096 idle connections at 64 KiB that is 256 MiB, acceptable
//     for this server and not for a hundred thousand clients. "Provided
//     buffers" and buffer rings exist for exactly this: the kernel picks a
//     buffer from a shared pool when data arrives. They are the first
//     experiment after this version.
//
//  3. Sends read from a buffer nothing touches until they complete.
//     Each connection has two: `sending`, which the kernel is reading, and
//     `pending`, where send() appends. A std::vector appended to while the
//     kernel reads it may reallocate, and the kernel would then read freed
//     memory: so `sending` is left strictly alone while a send is in flight.
//     When it has all gone, the two swap, and the old vector's capacity is
//     reused for the next batch.
//
//  4. One send in flight per connection.
//     Two sends on one socket may be carried out in either order, which would
//     break "in order". IOSQE_IO_LINK can chain them, but one at a time is
//     simpler, and everything queued in the meantime goes in the next one, so
//     the batching is free.
//
//  5. A short send.
//     The completion says how many bytes the kernel took; the rest is sent
//     again from where it stopped. The first attempt is made with
//     MSG_DONTWAIT, so a full socket answers at once (with -EAGAIN or a short
//     count) instead of waiting: that is the completion-side form of "the
//     socket would not take everything", and it is where blocked_writes is
//     counted. The retry goes without MSG_DONTWAIT, so the kernel waits for
//     room itself and completes it when there is some.
//
//  6. flush(id) is immediate.
//     It submits that connection's send (with MSG_DONTWAIT, so it cannot
//     wait) and then reaps. A send that can finish at once has finished
//     inside the submit, so its completion is there. Completions of sends
//     are acted on at once; everything else (receives, accepts) is set aside
//     in a stash and served, oldest first, by the next poll(). That way
//     nothing is dispatched to the handler out of turn, from inside one of
//     its own callbacks. Two consequences. A send that completes can leave
//     the next one prepared but not handed over (more was queued behind it,
//     or the socket filled and the retry must wait for room), so flush
//     submits again until a round prepares nothing. And poll() stashes all it
//     reaps, sends included, so a flush from inside a callback first takes
//     its own connection's send completion out of the stash, if one is
//     there: until then that send would look as if it were still in flight.
//
//  7. Two system calls a turn.
//     poll() is one io_uring_enter (submit what is queued and wait), and
//     flush_all() one more (submit every send of the turn at once). 100
//     clients each sending a request and getting an answer cost those two,
//     where epoll pays an epoll_wait plus a recv and a send per client.
//     Folding the sends into the next poll's enter would make it one, and
//     would hold every answer back until the next turn. On top of the two:
//     a setsockopt per new connection and a shutdown or close per ended one.
//
//  8. close(id) with operations in flight.
//     There is nearly always one: a receive waits on every idle connection.
//     close() takes the connection out of the table (the id is free at once)
//     and, if the kernel still holds any of its buffers, shuts the socket
//     down and moves the object to a list of "zombies". Closing the
//     descriptor would not end the waiting receive; shutdown does. Its
//     completions then arrive (0, an error, or -ECANCELED), are recognised
//     by the object's `closing` mark, and only counted; when the last one is
//     in, the object, its buffers and its socket go.
//
//  9. The destructor.
//     The same for every connection at once: shut every socket down, count
//     the completions flush() had set aside for the next poll (point 6:
//     each is an operation that has ended, but was not yet counted), cancel
//     the accept, and reap until every zombie has had its last completion.
//     Only then may the buffers go, which is why the ring is the last member
//     declared (destroyed first, after the destructor's body has drained
//     it). The wait is bounded (two seconds); should the kernel still hold
//     something after that, the remaining zombies are leaked on purpose:
//     memory the kernel may still write into must not be given back.
//
// 10. Accepting: one accept in flight, re-armed as soon as it completes.
//     Each completion is one connection, and the next accept is submitted at
//     the next io_uring_enter, so fifty clients connecting at once are
//     served one or two per turn: tens of turns, a few milliseconds, for a
//     gateway whose clients stay connected all day. Multishot accept (Linux
//     5.19) would take them all from one submission. The listener is made
//     blocking first: on older kernels an accept through the ring on a
//     non-blocking listener answers -EAGAIN at once instead of waiting.
//
// 11. The completion queue.
//     A connection has at most two operations in flight (a receive and a
//     send), and there is one accept, so 2 * connections + 1 completions can
//     be outstanding. The ring has 256 submission and 512 completion
//     entries. With more than about 255 busy connections the completion
//     queue can fill; the kernel then keeps what does not fit and the ring
//     brings it in at the next call (uring.hpp). A submission queue that is
//     full is submitted at once to make room. On an older kernel that
//     answers -EBUSY to a submit while completions are backed up, sqe()
//     throws instead of reaping and retrying: that case cannot be reached
//     safely from inside a reap callback, and has only been reasoned about.
//
// 12. Where io_uring is not allowed: available() puts one no-op through a
//     ring of its own (Uring::available); the constructor throws if the ring
//     cannot be made.
//
// NEXT EXPERIMENTS (docs/optimization-log.md, with the latency curve before
// and after each): multishot receive with a buffer ring (point 2);
// registered ("fixed") files; IORING_SETUP_DEFER_TASKRUN and SINGLE_ISSUER;
// SQPOLL, and what it costs in a core; zero-copy send, and the size below
// which it loses.

namespace obe::net {

class UringTransport {
 public:
    static constexpr std::string_view kName = "uring";
    static constexpr std::string_view kDescription =
        "completion: io_uring is told what to do and reports when it is done";

    [[nodiscard]] static bool available() noexcept { return Uring::available(); }

    explicit UringTransport(const TransportConfig& cfg = {})
        : read_chunk_(std::max<std::size_t>(cfg.read_chunk, 1)), ring_(kRingEntries) {}

    UringTransport(const UringTransport&) = delete;
    UringTransport& operator=(const UringTransport&) = delete;

    ~UringTransport() {
        // Design point 9: stop everything, then collect every completion, so
        // that no buffer is freed while the kernel may still use it.
        for (auto& [id, c] : connections_) {
            ::shutdown(c->fd.get(), SHUT_RDWR);
            c->closing = true;
            if (c->inflight != 0) {
                zombies_.push_back(std::move(c));
            }
        }
        connections_.clear();
        // Completions flush() set aside and poll() never served: each one is
        // an operation its connection still counts as in flight.
        while (!stash_.empty()) {
            const io_uring_cqe cqe = stash_.front();
            stash_.pop_front();
            settle(cqe);
        }
        if (accept_armed_) {
            if (io_uring_sqe* sqe = sqe_or_null()) {
                prep_cancel(sqe, kAcceptTag, kIgnoreTag);
            }
        }
        for (int spins = 0; spins < kDrainSpins && (!zombies_.empty() || accept_armed_); ++spins) {
            ring_.submit_and_wait(10);
            ring_.reap([this](const io_uring_cqe& cqe) { settle(cqe); });
        }
        for (std::unique_ptr<Conn>& zombie : zombies_) {
            // The kernel did not give these back in time. Their memory must
            // outlive any write it may still make: leak it.
            [[maybe_unused]] const Conn* leaked = zombie.release();
        }
    }

    void listen(const std::string& host, std::uint16_t port) {
        listener_ = listen_tcp(host, port);
        set_blocking(listener_.get());  // design point 10
        port_ = local_port(listener_.get());
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    template <TransportHandler H>
    std::size_t poll(int timeout_ms, H& handler) {
        arm_accept();
        std::size_t served = 0;
        if (stash_.empty()) {
            ring_.submit_and_wait(timeout_ms);
        } else {
            ring_.submit();  // something is already waiting to be served
        }
        // Completions set aside by flush(id) come first: they are older.
        served += serve_stash(handler);
        // Collected first and served after, so that a flush(id) from inside
        // a callback, which reaps, never runs inside this reap.
        ring_.reap([this](const io_uring_cqe& cqe) { stash_.push_back(cqe); });
        served += serve_stash(handler);
        report_dead(handler);
        return served;
    }

    void send(ConnId id, std::span<const std::byte> bytes) {
        const auto it = connections_.find(id);
        if (it == connections_.end() || it->second->state != LinkState::Up) {
            return;
        }
        Conn& c = *it->second;
        c.pending.insert(c.pending.end(), bytes.begin(), bytes.end());
        if (!c.dirty) {
            c.dirty = true;
            dirty_.push_back(id);
        }
    }

    template <TransportHandler H>
    void flush_all(H& handler) {
        // By index: nothing here adds to dirty_, but nothing should break if
        // something later did.
        // NOLINTNEXTLINE(modernize-loop-convert)
        for (std::size_t i = 0; i < dirty_.size(); ++i) {
            const auto it = connections_.find(dirty_[i]);
            if (it == connections_.end()) {
                continue;
            }
            Conn& c = *it->second;
            c.dirty = false;
            start_send(c);
        }
        dirty_.clear();
        // One call for all of them. A send to a socket with room finishes
        // inside it.
        ring_.submit();
        settle_sends();
        report_dead(handler);
    }

    LinkState flush(ConnId id) {
        const auto it = connections_.find(id);
        if (it == connections_.end()) {
            return LinkState::Failed;
        }
        Conn& c = *it->second;
        if (c.state == LinkState::Up) {
            // poll() may hold this connection's last send completion, behind
            // the receive whose callback is calling flush: then the send has
            // ended, but looks as if it were still in flight.
            settle_stashed_send(c);
            start_send(c);
            ring_.submit();
            settle_sends();
            // A completion can leave a new send prepared and not yet handed
            // over: the one that finished had more queued behind it, or found
            // the socket full and now waits for room. Hand those over too.
            // It stops when nothing new was prepared, at most a few rounds.
            for (int round = 0; round < kFlushRounds && ring_.unsubmitted() != 0; ++round) {
                ring_.submit();
                settle_sends();
            }
        }
        if (c.state != LinkState::Up) {
            c.reported = true;  // this return value is the report
        }
        return c.state;
    }

    [[nodiscard]] std::size_t unsent(ConnId id) const {
        const auto it = connections_.find(id);
        if (it == connections_.end()) {
            return 0;
        }
        const Conn& c = *it->second;
        return (c.sending.size() - c.sent) + c.pending.size();
    }

    void close(ConnId id) {
        const auto it = connections_.find(id);
        if (it == connections_.end()) {
            return;
        }
        std::unique_ptr<Conn> c = std::move(it->second);
        connections_.erase(it);
        c->closing = true;
        if (c->inflight != 0) {
            // Design point 8: the kernel still holds this connection's
            // buffers. Make its operations end, and keep the memory until
            // they have.
            ::shutdown(c->fd.get(), SHUT_RDWR);
            ++direct_syscalls_;
            zombies_.push_back(std::move(c));
        } else {
            ++direct_syscalls_;  // the close, as c goes out of scope
        }
    }

    [[nodiscard]] TransportStats stats() const noexcept {
        TransportStats s = stats_;
        s.syscalls = ring_.enters() + direct_syscalls_;
        return s;
    }

    [[nodiscard]] std::size_t connections() const noexcept { return connections_.size(); }

 private:
    enum Kind : std::uint64_t { kRecv = 0, kSend = 1 };
    static constexpr std::uint64_t kKindMask = 7;
    // Tags that are not connection addresses: a Conn is aligned to 8, so
    // its address is never 2 or 6.
    static constexpr std::uint64_t kAcceptTag = 2;
    static constexpr std::uint64_t kIgnoreTag = 6;

    static constexpr unsigned kRingEntries = 256;
    static constexpr int kDrainSpins = 200;  // of 10 ms each
    static constexpr int kFlushRounds = 4;

    struct Conn {
        Fd fd;
        ConnId id = 0;
        std::vector<std::byte> in;       // where a receive in flight lands
        std::vector<std::byte> sending;  // what a send in flight reads: never touched meanwhile
        std::size_t sent = 0;            // how much of `sending` the kernel has taken
        std::vector<std::byte> pending;  // queued behind it
        LinkState state = LinkState::Up;
        unsigned inflight = 0;  // operations the kernel holds
        bool recv_inflight = false;
        bool send_inflight = false;
        bool reported = false;
        bool closing = false;
        bool dirty = false;
        bool dead_listed = false;
    };
    static_assert(alignof(Conn) >= 8, "the low three bits of a Conn's address carry the kind");

    [[nodiscard]] static std::uint64_t tag(const Conn& c, Kind kind) noexcept {
        return reinterpret_cast<std::uint64_t>(&c) | kind;
    }
    [[nodiscard]] static Conn& conn_of(const io_uring_cqe& cqe) noexcept {
        // user_data is how the kernel hands the address back (design point 1).
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        return *reinterpret_cast<Conn*>(cqe.user_data & ~kKindMask);
    }
    [[nodiscard]] static bool is_send(const io_uring_cqe& cqe) noexcept {
        return (cqe.user_data & kKindMask) == kSend;
    }

    io_uring_sqe* sqe_or_null() {
        io_uring_sqe* sqe = ring_.get_sqe();
        if (sqe == nullptr) {
            ring_.submit();  // full: hand over what is there to make room
            sqe = ring_.get_sqe();
        }
        return sqe;
    }

    io_uring_sqe* sqe() {
        io_uring_sqe* s = sqe_or_null();
        if (s == nullptr) {
            throw std::runtime_error("io_uring: the submission queue will not drain");
        }
        return s;
    }

    void arm_accept() {
        if (!accept_armed_ && listener_.valid()) {
            prep_accept(sqe(), listener_.get(), SOCK_CLOEXEC, kAcceptTag);
            accept_armed_ = true;
        }
    }

    void arm_recv(Conn& c) {
        if (!c.recv_inflight && c.state == LinkState::Up && !c.closing) {
            prep_recv(sqe(), c.fd.get(), c.in.data(), c.in.size(), tag(c, kRecv));
            c.recv_inflight = true;
            ++c.inflight;
        }
    }

    // Starts the next send of a connection, if none is in flight (design
    // points 3 to 5).
    void start_send(Conn& c) {
        if (c.send_inflight || c.state != LinkState::Up || c.closing) {
            return;
        }
        if (c.sent == c.sending.size()) {
            c.sending.clear();
            c.sent = 0;
            c.sending.swap(c.pending);
        }
        if (c.sent == c.sending.size()) {
            return;  // nothing to send
        }
        prep_send(sqe(), c.fd.get(), c.sending.data() + c.sent, c.sending.size() - c.sent,
                  MSG_NOSIGNAL | MSG_DONTWAIT, tag(c, kSend));
        c.send_inflight = true;
        ++c.inflight;
    }

    // Sends the rest of `sending` and lets the kernel wait for room.
    void resend_waiting(Conn& c) {
        prep_send(sqe(), c.fd.get(), c.sending.data() + c.sent, c.sending.size() - c.sent,
                  MSG_NOSIGNAL, tag(c, kSend));
        c.send_inflight = true;
        ++c.inflight;
    }

    void mark_dead(Conn& c, LinkState why) {
        if (c.state == LinkState::Up) {
            c.state = why;
            if (!c.dead_listed && !c.closing) {
                c.dead_listed = true;
                dead_.push_back(c.id);
            }
        }
    }

    // One of the connection's operations has ended. A closed connection is
    // freed with its last one.
    void done_with(Conn& c) {
        --c.inflight;
        if (c.closing && c.inflight == 0) {
            const auto it = std::find_if(zombies_.begin(), zombies_.end(),
                                         [&c](const auto& z) { return z.get() == &c; });
            if (it != zombies_.end()) {
                ++direct_syscalls_;  // the close
                zombies_.erase(it);
            }
        }
    }

    // A send completed.
    void settle_send(Conn& c, int res) {
        c.send_inflight = false;
        if (!c.closing) {
            if (res >= 0) {
                c.sent += static_cast<std::size_t>(res);
                stats_.bytes_out += static_cast<std::uint64_t>(res);
                if (c.sent < c.sending.size()) {
                    // The socket took only part: it is full. Wait for room
                    // this time instead of asking again and again.
                    ++stats_.blocked_writes;
                    resend_waiting(c);
                } else if (!c.pending.empty()) {
                    start_send(c);
                }
            } else if (res == -EAGAIN) {
                ++stats_.blocked_writes;
                resend_waiting(c);
            } else if (res == -EINTR || res == -ECANCELED) {
                // Ended without an answer, on a connection that is still
                // open: nothing else would ever send the rest.
                resend_waiting(c);
            } else {
                mark_dead(c, res == -EPIPE || res == -ECONNRESET ? LinkState::ClosedByPeer
                                                                 : LinkState::Failed);
            }
        }
        done_with(c);
    }

    // If poll() has set aside a completion of this connection's send, takes
    // it out of the queue and acts on it now. A send completion calls no
    // handler, so acting on it early changes nothing anyone has been told.
    void settle_stashed_send(Conn& c) {
        if (!c.send_inflight) {
            return;
        }
        const std::uint64_t mine = tag(c, kSend);
        const auto it = std::find_if(stash_.begin(), stash_.end(), [mine](const io_uring_cqe& cqe) {
            return cqe.user_data == mine;
        });
        if (it != stash_.end()) {
            const int res = it->res;
            stash_.erase(it);
            settle_send(c, res);
        }
    }

    // For the destructor: only bookkeeping.
    void settle(const io_uring_cqe& cqe) {
        if (cqe.user_data == kAcceptTag) {
            accept_armed_ = false;
            if (cqe.res >= 0) {
                ::close(cqe.res);
            }
            return;
        }
        if (cqe.user_data == kIgnoreTag) {
            return;
        }
        Conn& c = conn_of(cqe);
        if (is_send(cqe)) {
            c.send_inflight = false;
        } else {
            c.recv_inflight = false;
        }
        done_with(c);
    }

    // Takes the completions that are there: acts on the sends, and sets the
    // rest aside for poll (design point 6).
    void settle_sends() {
        ring_.reap([this](const io_uring_cqe& cqe) {
            if (cqe.user_data != kAcceptTag && cqe.user_data != kIgnoreTag && is_send(cqe)) {
                settle_send(conn_of(cqe), cqe.res);
            } else {
                stash_.push_back(cqe);
            }
        });
    }

    template <class H>
    std::size_t serve_stash(H& handler) {
        std::size_t served = 0;
        while (!stash_.empty()) {
            const io_uring_cqe cqe = stash_.front();
            stash_.pop_front();
            serve(cqe, handler);
            ++served;
        }
        return served;
    }

    template <class H>
    void serve(const io_uring_cqe& cqe, H& handler) {
        if (cqe.user_data == kIgnoreTag) {
            return;
        }
        if (cqe.user_data == kAcceptTag) {
            accept_armed_ = false;
            if (cqe.res >= 0) {
                accepted(Fd(cqe.res), handler);
            }
            arm_accept();
            return;
        }
        Conn& c = conn_of(cqe);
        if (is_send(cqe)) {
            settle_send(c, cqe.res);
            return;
        }
        c.recv_inflight = false;
        if (!c.closing && c.state == LinkState::Up) {
            if (cqe.res > 0) {
                stats_.bytes_in += static_cast<std::uint64_t>(cqe.res);
                handler.on_data(c.id, std::span<const std::byte>(
                                          c.in.data(), static_cast<std::size_t>(cqe.res)));
                arm_recv(c);
            } else if (cqe.res == 0 || cqe.res == -ECONNRESET) {
                mark_dead(c, LinkState::ClosedByPeer);
            } else if (cqe.res == -EINTR || cqe.res == -EAGAIN || cqe.res == -ECANCELED) {
                arm_recv(c);  // ended without an answer on an open connection: ask again
            } else {
                mark_dead(c, LinkState::Failed);
            }
        }
        done_with(c);
    }

    template <class H>
    void accepted(Fd fd, H& handler) {
        const ConnId id = handler.on_connect();
        if (id == 0) {
            ++direct_syscalls_;  // the close, as fd goes out of scope
            return;
        }
        const int one = 1;
        ::setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ++direct_syscalls_;
        auto c = std::make_unique<Conn>();
        c->fd = std::move(fd);
        c->id = id;
        c->in.resize(read_chunk_);
        Conn& ref = *c;
        connections_[id] = std::move(c);
        arm_recv(ref);
    }

    template <class H>
    void report_dead(H& handler) {
        // NOLINTNEXTLINE(modernize-loop-convert)
        for (std::size_t i = 0; i < dead_.size(); ++i) {
            const auto it = connections_.find(dead_[i]);
            if (it == connections_.end()) {
                continue;
            }
            Conn& c = *it->second;
            c.dead_listed = false;
            if (c.state != LinkState::Up && !c.reported) {
                c.reported = true;
                handler.on_disconnect(c.id, c.state);
            }
        }
        dead_.clear();
    }

    std::size_t read_chunk_;
    Fd listener_;
    std::uint16_t port_ = 0;
    std::unordered_map<ConnId, std::unique_ptr<Conn>> connections_;
    std::vector<std::unique_ptr<Conn>> zombies_;  // closed, with operations still in flight
    std::vector<ConnId> dirty_;                   // connections with output queued this turn
    std::vector<ConnId> dead_;                    // connections to report as ended
    std::deque<io_uring_cqe> stash_;              // completions set aside by flush
    bool accept_armed_ = false;
    TransportStats stats_{};
    std::uint64_t direct_syscalls_ = 0;  // calls made outside the ring
    Uring ring_;                         // last: destroyed first (design point 9)
};

static_assert(Transport<UringTransport>);

}  // namespace obe::net
