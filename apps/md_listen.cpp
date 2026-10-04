// md_listen: subscribe to the exchange's market data and rebuild the books.
//
//   md_listen [options]
//     --md HOST:PORT      where the feed is published (default 239.255.42.1:9002)
//     --md-interface IP   local address to join a multicast group on
//                         (default 127.0.0.1)
//     --session NAME      session name to accept (default OBE)
//     --seconds N         stop after N seconds (default: run until the session
//                         ends or a signal arrives)
//
// It receives the MoldUDP64-style packets exchange_server sends, checks their
// sequence numbers, and feeds the ITCH messages inside to the same BookManager
// that replays a Nasdaq file. That is the last link of the round trip: orders
// in over TCP, matched, published over UDP, and rebuilt into a book by code
// that knows nothing about the engine.
//
// It prints "listening" once its socket is bound. A subscriber that starts
// after the exchange has missed the Stock Directory and every order already
// in the book, and has no way to ask for them: this project has no snapshot
// or re-request service. Start it first.
//
// Exit status: 0 the stream was complete and the book consistent, 1 usage or
// set-up error, 2 messages were missed (a sequence gap), 3 the stream was
// complete but the book broke an invariant.

#include <sys/epoll.h>

#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/net/epoll_loop.hpp"
#include "obe/net/moldudp.hpp"
#include "obe/net/socket.hpp"
#include "obe/net/udp.hpp"
#include "obe/util/format.hpp"

namespace {

using namespace obe;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int /*signal*/) {
    g_stop = 1;
}

struct Options {
    net::Endpoint md{"239.255.42.1", 9002};
    std::string md_interface = "127.0.0.1";
    std::string session = "OBE";
    std::uint64_t seconds = 0;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: md_listen [--md HOST:PORT] [--md-interface IP] [--session NAME] "
                 "[--seconds N]\n");
    return status;
}

void line(const char* label, std::uint64_t value) {
    std::printf("  %-32s %s\n", label, util::with_commas(value).c_str());
}

int run(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = i + 1 < argc;
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--md" && has_next) {
            if (!net::parse_endpoint(argv[++i], opt.md)) {
                std::fprintf(stderr, "error: --md needs HOST:PORT with an IPv4 address\n");
                return 1;
            }
        } else if (arg == "--md-interface" && has_next) {
            opt.md_interface = argv[++i];
        } else if (arg == "--session" && has_next) {
            opt.session = argv[++i];
        } else if (arg == "--seconds" && has_next) {
            const std::string_view text = argv[++i];
            const auto r = std::from_chars(text.data(), text.data() + text.size(), opt.seconds);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) {
                std::fprintf(stderr, "error: --seconds needs a whole number\n");
                return 1;
            }
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }

    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    using Manager = book::BookManager<book::OrderStore, book::PriceLevels>;
    const auto manager = std::make_unique<Manager>();
    net::MoldReceiver<Manager> receiver(net::make_session(opt.session), *manager);
    net::UdpReceiver socket(opt.md.host, opt.md.port, opt.md_interface);
    net::EpollLoop loop;
    loop.add(socket.fd(), EPOLLIN, 1);

    std::printf("listening on %s:%u, session '%s'\n", opt.md.host.c_str(),
                static_cast<unsigned>(socket.port()), opt.session.c_str());
    std::fflush(stdout);

    const auto start = std::chrono::steady_clock::now();
    std::array<std::byte, 65'536> datagram{};
    while (g_stop == 0 && !receiver.stats().ended) {
        if (opt.seconds != 0 &&
            std::chrono::steady_clock::now() - start >= std::chrono::seconds(opt.seconds)) {
            break;
        }
        static_cast<void>(loop.wait(100));
        // Take everything that is queued before waiting again.
        for (;;) {
            const std::ptrdiff_t n = socket.receive(datagram);
            if (n < 0) {
                break;
            }
            receiver.on_packet({datagram.data(), static_cast<std::size_t>(n)});
        }
    }

    const net::MoldStats& s = receiver.stats();
    const book::Counters& counters = manager->counters();
    const book::Audit audit = manager->audit();
    const book::Stats& stats = manager->stats();
    std::printf("\nmd_listen\n");
    line("packets", s.packets);
    line("messages", s.messages);
    line("heartbeats", s.heartbeats);
    line("sequence gaps", s.gaps);
    line("messages missed", s.missed_messages);
    line("duplicate messages skipped", s.duplicates);
    line("malformed packets", s.bad_packets);
    line("malformed messages", s.bad_messages);
    line("packets of another session", s.other_sessions);
    line("best bid/offer updates", stats.bbo_updates);
    line("orders resting", audit.open_orders);
    line("price levels", audit.levels);
    line("locked or crossed updates", stats.locked_while_trading + stats.locked_while_not_trading +
                                          stats.crossed_while_trading +
                                          stats.crossed_while_not_trading);
    std::printf("  %-32s %s\n", "session ended by the sender", s.ended ? "yes" : "no");

    if (!receiver.complete()) {
        std::printf("\nRESULT: %s messages were missed; the book is not to be trusted\n",
                    util::with_commas(s.missed_messages).c_str());
        return 2;
    }
    if (!counters.clean() || !audit.clean() || s.bad_packets != 0 || s.bad_messages != 0) {
        std::printf("\nRESULT: the stream was complete but the book broke an invariant\n");
        return 3;
    }
    std::printf("\nRESULT: complete and consistent\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
