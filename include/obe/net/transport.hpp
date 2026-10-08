#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// A transport: the part of a TCP server that talks to the kernel.
//
// The order gateway (order_gateway.hpp) is two things that have nothing to do
// with each other. One is the session: which bytes make a request, what the
// engine is asked, whose report goes where, when a client is too slow to
// keep. The other is the plumbing: accepting sockets, getting bytes out of
// them and into them, and finding out which of ten thousand has something to
// say. This file is the line between the two, so that the plumbing can be
// done in two ways and the session written once.
//
// TWO WAYS TO DO I/O
//
// Readiness (epoll_transport.hpp). The kernel is asked "which of these
// sockets could I read or write without waiting?", answers with a list, and
// the program then makes one system call per socket to do the reading or the
// writing itself. The program owns every buffer the whole time: a recv()
// copies into memory the program chose at that moment.
//
// Completion (io_uring: uring_transport.hpp). The program says in advance
// "read from this socket into this buffer", for many sockets at once, and the
// kernel answers later with "done, 512 bytes". Requests and answers travel
// through two rings of memory shared with the kernel, so a whole turn of the
// event loop, however many sockets were busy in it, can cost a single system
// call. In exchange the kernel holds a pointer into the program's memory from
// the moment of asking until the answer comes back, and nothing may move or
// free that memory in between.
//
// The same server, the same clients and the same bytes on the wire: what
// differs is how many times the program crosses into the kernel per request,
// and who owns a buffer while an operation is under way. `transport_bench`
// measures the first. The tests in tests/net/transport_test.cpp are about
// the second.
//
// THE CONTRACT
//
// A transport serves any number of connections on one thread. It names each
// by a ConnId, which its user chooses (see on_connect below).
//
//   listen(host, port)        Starts accepting. Port 0 lets the system pick;
//                             port() says which it picked.
//
//   poll(timeout_ms, handler) One turn: waits up to timeout_ms for something
//                             to happen and tells the handler about each
//                             thing that did (below). Returns the number of
//                             things it served; 0 means the time ran out.
//
//   send(id, bytes)           Queues bytes for a connection, in order after
//                             everything queued before. Never waits and never
//                             fails: the bytes are copied, and the caller's
//                             buffer is free again when send returns. Bytes
//                             for a connection that is dead or unknown are
//                             dropped. Queuing is all it does: send itself
//                             offers nothing to the kernel. Bytes leave at
//                             the next flush_all or flush(id), or, behind a
//                             backlog, when the socket has room again. So
//                             what is queued for a connection with no
//                             backlog, which is then closed before a flush,
//                             never leaves.
//
//   flush_all(handler)        Starts everything queued on every connection on
//                             its way: whatever the kernel takes without
//                             waiting has been handed to it before this
//                             returns. Called at the end of each turn. It
//                             may report connections it finds dead through
//                             handler.on_disconnect.
//
//   flush(id)                 The same for one connection, for a caller that
//                             needs to know NOW how far behind a client is.
//                             On return unsent(id) is what the kernel would
//                             not take. Returns the state of the connection;
//                             a dead one is reported this way and then not
//                             again through on_disconnect.
//
//   unsent(id)                Bytes queued for the connection that the kernel
//                             has not yet taken. 0 for an unknown id.
//
//   close(id)                 Forgets the connection and closes its socket.
//                             Bytes still unsent are discarded. Nothing is
//                             reported about the id afterwards, and the id
//                             may be given to a new connection. Unknown ids
//                             are ignored.
//
// What a transport tells its handler, during poll:
//
//   on_connect() -> ConnId    A client connected. The handler returns the id
//                             this connection is to be known by, which must
//                             not be 0 or the id of a connection it has not
//                             closed; or 0 to refuse, and the transport
//                             closes the socket.
//
//   on_data(id, bytes)        Bytes arrived, in order. They are valid only
//                             during the call. Where one batch ends and the
//                             next begins means nothing: TCP is a stream.
//
//   on_disconnect(id, why)    The client closed its end or the socket
//                             failed. Told once for a connection, and never
//                             after close(id). Nothing more will arrive on
//                             it and what is sent to it is dropped. The id
//                             stays taken until the handler calls close(id),
//                             which it must, sooner or later, for every
//                             connection it accepted.
//
// Inside a callback the handler may call send, flush and unsent, for any
// connection. It must not call poll, flush_all or close: connections are
// closed between turns.
//
// Everything queued before a connection dies is either delivered in order or
// not at all from some point on. Nothing is ever delivered twice, out of
// order, or to another connection.

namespace obe::net {

// The name a connection goes by. The gateway uses the owner id it gives the
// connection's orders, so the two are one number.
using ConnId = std::uint32_t;

enum class LinkState : std::uint8_t {
    Up,            // as far as the transport knows, both ends are there
    ClosedByPeer,  // the client closed, cleanly or with a reset
    Failed,        // any other socket error
};

struct TransportConfig {
    // Most bytes taken from one connection in one piece. A bound keeps one
    // flooding client from starving the others of a turn.
    std::size_t read_chunk = std::size_t{64} << 10;
};

struct TransportStats {
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;
    // Times a socket would not take everything it was offered at once: a
    // client that is not reading as fast as it is written to.
    std::uint64_t blocked_writes = 0;
    // Crossings into the kernel while serving: waiting, accepting, reading,
    // writing, closing. Setting up (the listening socket, the rings) is not
    // counted. Per request served, this is the number the two ways of doing
    // I/O differ by.
    std::uint64_t syscalls = 0;

    friend bool operator==(const TransportStats&, const TransportStats&) = default;
};

template <class H>
concept TransportHandler = requires(H handler, ConnId id, std::span<const std::byte> bytes) {
    { handler.on_connect() } -> std::same_as<ConnId>;
    handler.on_data(id, bytes);
    handler.on_disconnect(id, LinkState::ClosedByPeer);
};

// The smallest handler: refuses everybody. It is what the concept below is
// checked with, and is of no other use.
struct NoHandler {
    ConnId on_connect() { return 0; }
    void on_data(ConnId /*id*/, std::span<const std::byte> /*bytes*/) {}
    void on_disconnect(ConnId /*id*/, LinkState /*why*/) {}
};

template <class T>
concept Transport = std::constructible_from<T, const TransportConfig&> &&
                    requires(T transport, const T constant, NoHandler handler, ConnId id,
                             std::span<const std::byte> bytes, const std::string& host) {
                        { T::kName } -> std::convertible_to<std::string_view>;
                        { T::kDescription } -> std::convertible_to<std::string_view>;
                        // Whether this kernel can run it at all. A transport
                        // that is not available throws when constructed.
                        { T::available() } -> std::same_as<bool>;
                        transport.listen(host, std::uint16_t{0});
                        { constant.port() } -> std::same_as<std::uint16_t>;
                        { transport.poll(0, handler) } -> std::same_as<std::size_t>;
                        transport.send(id, bytes);
                        transport.flush_all(handler);
                        { transport.flush(id) } -> std::same_as<LinkState>;
                        { constant.unsent(id) } -> std::same_as<std::size_t>;
                        transport.close(id);
                        { constant.stats() } -> std::convertible_to<TransportStats>;
                    };

}  // namespace obe::net
