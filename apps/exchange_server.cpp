// exchange_server: a small exchange on one thread.
//
//   exchange_server [options]
//     --listen HOST:PORT  where to accept order-entry connections
//                         (default 127.0.0.1:9001; port 0 picks a free one)
//     --md HOST:PORT      where to publish market data over UDP
//                         (default 239.255.42.1:9002, a multicast group;
//                         an ordinary address such as 127.0.0.1:9002 works too)
//     --md-interface IP   local address multicast leaves from (default 127.0.0.1)
//     --session NAME      market-data session name, up to 10 characters
//     --symbols N         instruments to open, S00001 onwards (default 16)
//     --engine NAME       which matching engine (default reference)
//     --io NAME           how sockets are served: epoll (default) or uring
//     --list-io           print the transport names and whether each can run
//     --keep-orders       do not cancel a client's orders when it disconnects
//     --max-pending N     bytes queued for a slow client before it is dropped
//     --quiet             no statistics at exit
//
// Clients speak the protocol in obe/net/protocol.hpp over TCP. Every change to
// a book is published as ITCH messages in MoldUDP64-style packets
// (obe/net/moldudp.hpp). load_gen is a client; md_listen is a subscriber.
//
// It prints "listening on HOST:PORT" once it is ready, which is what a script
// that asked for port 0 should wait for and read. It runs until it receives
// SIGINT or SIGTERM, then tells subscribers the session has ended.
//
// Exit status: 0 stopped by a signal, 1 usage or set-up error, 4 the chosen
// engine or transport is not written yet, 5 the chosen transport cannot run
// on this system (--list-io says which can).

#include <charconv>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "obe/engine/engines.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/feed/messages.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/net/moldudp.hpp"
#include "obe/net/order_gateway.hpp"
#include "obe/net/socket.hpp"
#include "obe/net/transports.hpp"
#include "obe/net/udp.hpp"
#include "obe/util/format.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;

// Set by the signal handler, read by the event loop. sig_atomic_t is the one
// type a handler may write.
volatile std::sig_atomic_t g_stop = 0;

void on_signal(int /*signal*/) {
    g_stop = 1;
}

struct Options {
    net::Endpoint listen{"127.0.0.1", 9001};
    net::Endpoint md{"239.255.42.1", 9002};
    std::string md_interface = "127.0.0.1";
    std::string session = "OBE";
    std::string engine = "reference";
    std::string io = "epoll";
    std::uint32_t symbols = 16;
    net::GatewayConfig gateway;
    bool quiet = false;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: exchange_server [--listen HOST:PORT] [--md HOST:PORT] "
                 "[--md-interface IP]\n"
                 "                       [--session NAME] [--symbols N] [--engine NAME] "
                 "[--io NAME]\n"
                 "                       [--keep-orders] [--max-pending BYTES] [--quiet]\n"
                 "       exchange_server --list-io\n");
    return status;
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

void line(const char* label, std::uint64_t value) {
    std::printf("  %-32s %s\n", label, util::with_commas(value).c_str());
}

// Sends each finished packet to the UDP socket.
struct PacketSender {
    net::UdpSender* socket;
    void operator()(std::span<const std::byte> packet) const {
        static_cast<void>(socket->send(packet));
    }
};

template <class Impl, class Net>
int serve(const Options& opt) {
    using Publisher = net::MoldPacketizer<PacketSender>;
    using Gateway = net::OrderGateway<Impl, Publisher, Net>;

    if (!Net::available()) {
        std::fprintf(stderr,
                     "error: the '%.*s' transport cannot run here: the kernel is too old for "
                     "it, or this system does not allow it\n",
                     static_cast<int>(Net::kName.size()), Net::kName.data());
        return 5;
    }

    net::UdpSender socket(opt.md.host, opt.md.port, opt.md_interface);
    Publisher publisher(net::make_session(opt.session), PacketSender{&socket});
    Gateway gateway(publisher, opt.gateway);

    publisher.on_system_event(
        feed::SystemEvent{.hdr = engine::md::header(0, gateway.now()), .event_code = 'O'});
    for (std::uint32_t i = 1; i <= opt.symbols; ++i) {
        const auto locate = static_cast<Locate>(i);
        gateway.engine().add_instrument(locate, gen::flow_symbol(locate), gateway.now());
    }
    publisher.on_system_event(
        feed::SystemEvent{.hdr = engine::md::header(0, gateway.now()), .event_code = 'Q'});
    publisher.flush();

    gateway.listen(opt.listen.host, opt.listen.port);
    std::printf("listening on %s:%u\n", opt.listen.host.c_str(),
                static_cast<unsigned>(gateway.port()));
    std::printf("market data to %s:%u, session '%s', %u symbols, engine %.*s, io %.*s\n",
                opt.md.host.c_str(), static_cast<unsigned>(opt.md.port), opt.session.c_str(),
                opt.symbols, static_cast<int>(Impl::kName.size()), Impl::kName.data(),
                static_cast<int>(Net::kName.size()), Net::kName.data());
    std::fflush(stdout);

    // A heartbeat once a second when nothing else was published, so that a
    // subscriber can tell a quiet market from a dead feed, and learns of a
    // packet lost at the end of a burst.
    auto last_packet = std::chrono::steady_clock::now();
    std::uint64_t packets_seen = publisher.packets();
    while (g_stop == 0) {
        gateway.poll(100);
        // Everything this turn's orders published travels now.
        publisher.flush();
        const auto time = std::chrono::steady_clock::now();
        if (publisher.packets() != packets_seen) {
            packets_seen = publisher.packets();
            last_packet = time;
        } else if (time - last_packet >= std::chrono::seconds(1)) {
            publisher.heartbeat();
            packets_seen = publisher.packets();
            last_packet = time;
        }
    }

    publisher.on_system_event(
        feed::SystemEvent{.hdr = engine::md::header(0, gateway.now()), .event_code = 'C'});
    publisher.end_of_session();

    if (!opt.quiet) {
        const net::GatewayStats& s = gateway.stats();
        const engine::EngineStats& e = gateway.engine().stats();
        std::printf("\nexchange_server stopped\n");
        line("connections accepted", s.connections_accepted);
        line("connections refused", s.connections_refused);
        line("closed by the client", s.closed_by_peer);
        line("closed: not a valid request", s.closed_protocol);
        line("closed: too slow reading", s.closed_slow);
        line("closed: socket error", s.closed_error);
        line("requests", s.requests);
        line("responses", s.responses);
        line("responses to departed clients", s.responses_dropped);
        line("bytes in", s.bytes_in);
        line("bytes out", s.bytes_out);
        line("writes a socket would not take", s.blocked_writes);
        const std::uint64_t syscalls = gateway.transport().stats().syscalls;
        line("system calls while serving", syscalls);
        if (s.requests != 0) {
            std::printf("  %-32s %.2f\n", "system calls per request",
                        static_cast<double>(syscalls) / static_cast<double>(s.requests));
        }
        line("orders cancelled on disconnect", s.orders_cancelled_on_disconnect);
        line("orders accepted", e.accepted);
        line("requests rejected", e.rejected);
        line("trades", e.trades);
        line("orders resting at the end", gateway.engine().open_orders());
        line("market-data messages", publisher.messages());
        line("market-data packets", publisher.packets());
        line("packets the kernel refused", socket.dropped());
    }
    return 0;
}

int run(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = i + 1 < argc;
        std::uint64_t value = 0;
        const bool has_value = has_next && parse_u64(argv[i + 1], value);
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--quiet") {
            opt.quiet = true;
        } else if (arg == "--keep-orders") {
            opt.gateway.cancel_on_disconnect = false;
        } else if (arg == "--listen" && has_next) {
            if (!net::parse_endpoint(argv[++i], opt.listen)) {
                std::fprintf(stderr, "error: --listen needs HOST:PORT with an IPv4 address\n");
                return 1;
            }
        } else if (arg == "--md" && has_next) {
            if (!net::parse_endpoint(argv[++i], opt.md)) {
                std::fprintf(stderr, "error: --md needs HOST:PORT with an IPv4 address\n");
                return 1;
            }
        } else if (arg == "--md-interface" && has_next) {
            opt.md_interface = argv[++i];
        } else if (arg == "--session" && has_next) {
            opt.session = argv[++i];
        } else if (arg == "--engine" && has_next) {
            opt.engine = argv[++i];
        } else if (arg == "--io" && has_next) {
            opt.io = argv[++i];
        } else if (arg == "--list-io") {
            net::for_each_transport([]<class Net>(std::type_identity<Net>) {
                std::printf("%-8.*s %-20s %.*s\n", static_cast<int>(Net::kName.size()),
                            Net::kName.data(),
                            Net::available() ? "(can run here)" : "(cannot run here)",
                            static_cast<int>(Net::kDescription.size()), Net::kDescription.data());
            });
            return 0;
        } else if (arg == "--symbols" && has_value) {
            opt.symbols = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--max-pending" && has_value) {
            opt.gateway.max_pending_output = static_cast<std::size_t>(value);
            ++i;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (opt.symbols == 0 || opt.symbols > 60'000) {
        std::fprintf(stderr, "error: --symbols must be between 1 and 60000\n");
        return 1;
    }

    // No SA_RESTART: a signal must interrupt epoll_wait so the loop can stop.
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    int status = 1;
    bool known_io = false;
    const bool known = engine::with_engine(opt.engine, [&]<class Impl>(std::type_identity<Impl>) {
        known_io = net::with_transport(
            opt.io, [&]<class Net>(std::type_identity<Net>) { status = serve<Impl, Net>(opt); });
    });
    if (!known) {
        std::fprintf(stderr, "error: no engine called '%s'. Try flow_gen --list.\n",
                     opt.engine.c_str());
        return 1;
    }
    if (!known_io) {
        std::fprintf(stderr, "error: no transport called '%s'. Try --list-io.\n", opt.io.c_str());
        return 1;
    }
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const obe::util::Unimplemented& e) {
        std::fprintf(stderr, "not built yet: %s\n", e.what());
        return 4;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
