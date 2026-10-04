// load_gen: send orders to exchange_server at a fixed rate and measure how
// long each takes to be acknowledged.
//
//   load_gen [options]
//     --connect HOST:PORT   the gateway (default 127.0.0.1:9001)
//     --connections N       TCP connections to spread the orders over (default 8)
//     --rate N[,N,...]      requests per second, over all connections together.
//                           Several rates are run one after the other and give
//                           the latency-versus-throughput curve (default 10000)
//     --seconds N           measured seconds at each rate (default 5)
//     --warmup N            unmeasured seconds before them, at each rate (default 1)
//     --symbols N           instruments the server has open (default 16)
//     --live N              resting orders the flow hovers around (default 2000)
//     --seed N              seed of the order flow (default 1)
//     --cpu N               pin this thread to CPU N
//     --json FILE           also write the results as JSON
//     --label TEXT          free text carried into the report
//
// The requests are the same seeded flow that drives the engine in the tests
// (obe/gen/order_flow.hpp): new orders of every kind, cancels and replaces,
// around a moving mid price. Here it travels over TCP, and the generator
// learns what is resting from the acknowledgements that come back.
//
// THE POINT OF THIS PROGRAM IS HOW IT MEASURES.
//
// The schedule is fixed in advance: request number i is due at
// start + i / rate, whatever happens. Its latency is counted from that due
// time to the moment its acknowledgement is read, not from the moment it was
// actually written to the socket.
//
// The difference matters when the server stalls. A generator that sends, waits
// for the answer, and only then sends the next request slows down with the
// server: during a one-second stall it sends one request, records one slow
// sample, and the thousands of requests that would have arrived in that second
// are never sent and never counted. The report then shows one bad sample in a
// sea of good ones, and the stall has all but vanished. That is called
// coordinated omission. With a fixed schedule those requests fall due anyway,
// each is late by however long it waited, and the stall shows up in the
// percentiles at its true size.
//
// The price is that a generator which cannot keep to the schedule itself adds
// its own lateness to the numbers. The report gives the worst lag behind
// schedule so that this is visible: if it is large, the generator was the
// bottleneck and the latencies are not the server's.
//
// Both programs here run on one machine and talk over loopback. No packet ever
// reaches a network card, so these numbers measure the software path (system
// calls, the kernel's TCP stack, the gateway, the engine) and say nothing
// about a network.
//
// Exit status: 0 every request was acknowledged, 1 usage or connection error,
// 2 some requests were never acknowledged.

#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "obe/engine/types.hpp"
#include "obe/feed/codec.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/net/epoll_loop.hpp"
#include "obe/net/open_loop.hpp"
#include "obe/net/protocol.hpp"
#include "obe/net/socket.hpp"
#include "obe/util/backoff.hpp"
#include "obe/util/cpu.hpp"
#include "obe/util/format.hpp"
#include "obe/util/latency_histogram.hpp"

namespace {

using namespace obe;

struct Options {
    net::Endpoint connect{"127.0.0.1", 9001};
    std::uint32_t connections = 8;
    std::vector<std::uint64_t> rates{10'000};
    std::uint64_t seconds = 5;
    std::uint64_t warmup = 1;
    gen::FlowConfig flow{.symbols = 16, .target_live_orders = 2'000};
    std::string json_path;
    std::string label;
    int cpu = -1;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: load_gen [--connect HOST:PORT] [--connections N] [--rate N[,N...]]\n"
                 "                [--seconds N] [--warmup N] [--symbols N] [--live N] [--seed N]\n"
                 "                [--cpu N] [--json FILE] [--label TEXT]\n");
    return status;
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

bool parse_rates(std::string_view text, std::vector<std::uint64_t>& out) {
    out.clear();
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        std::uint64_t rate = 0;
        if (!parse_u64(text.substr(0, comma), rate) || rate == 0) {
            return false;
        }
        out.push_back(rate);
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return !out.empty();
}

[[nodiscard]] std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// ---------------------------------------------------------------------------
// One connection to the gateway
// ---------------------------------------------------------------------------

struct Connection {
    net::Fd fd;
    std::vector<std::byte> out;  // requests not yet written
    std::size_t out_sent = 0;
    std::vector<std::byte> in;  // the start of a response still arriving
    // The requests sent on this connection and not yet acknowledged
    // (obe/net/open_loop.hpp says why no token is needed to match them).
    net::AckQueue pending;
};

// What one rate produced.
struct RateResult {
    std::uint64_t target_rate = 0;
    double measured_seconds = 0;
    std::uint64_t sent = 0;          // requests in the measured window
    std::uint64_t acknowledged = 0;  // of those
    std::uint64_t rejected = 0;      // acknowledgements that were rejections
    std::uint64_t executions = 0;    // Executed messages received, both sides
    std::uint64_t unanswered = 0;    // still waiting when the run gave up
    std::uint64_t worst_lag_ns = 0;  // how late the generator itself was at worst
    util::LatencyHistogram latency;  // nanoseconds, due time to acknowledgement

    [[nodiscard]] double achieved_rate() const {
        return measured_seconds > 0 ? static_cast<double>(sent) / measured_seconds : 0;
    }
};

class LoadGenerator {
 public:
    explicit LoadGenerator(const Options& opt) : opt_(opt), flow_(flow_config(opt)) {
        connections_.resize(opt.connections);
        for (std::uint32_t i = 0; i < opt.connections; ++i) {
            connections_[i].fd = net::connect_tcp(opt.connect.host, opt.connect.port, true);
            loop_.add(connections_[i].fd.get(), EPOLLIN, i);
        }
        scratch_.resize(std::size_t{64} << 10);
    }

    // Sends at `rate` for the warm-up and then the measured time, waits for
    // the last acknowledgements, and returns what the measured part showed.
    std::unique_ptr<RateResult> run(std::uint64_t rate) {
        auto result = std::make_unique<RateResult>();
        result->target_rate = rate;
        result_ = result.get();

        constexpr std::uint64_t kSecond = 1'000'000'000ULL;
        const std::uint64_t start = now_ns() + 1'000'000;  // one millisecond from now
        measure_from_ = start + opt_.warmup * kSecond;
        const std::uint64_t stop = measure_from_ + opt_.seconds * kSecond;
        const net::OpenLoopSchedule schedule(start, rate);

        std::uint64_t next = 0;
        std::uint64_t next_due = schedule.due(0);
        util::Backoff idle;
        for (;;) {
            const std::uint64_t now = now_ns();
            bool worked = false;
            // Everything that has fallen due, but not so much at once that
            // reading acknowledgements is put off for long.
            for (int burst = 0; burst < 64 && next_due <= now && next_due < stop; ++burst) {
                send_one(next_due, now);
                next_due = schedule.due(++next);
                worked = true;
            }
            worked = write_pending() || worked;
            worked = read_ready() || worked;

            if (next_due >= stop) {
                if (waiting_ == 0) {
                    break;
                }
                // Give a stalled server a while to answer, then report what
                // is still missing instead of waiting for ever.
                if (now_ns() > stop + 5 * kSecond) {
                    result->unanswered = waiting_;
                    break;
                }
            }
            if (worked) {
                idle.reset();
            } else {
                idle.pause();
            }
        }
        result->measured_seconds = static_cast<double>(opt_.seconds);
        result_ = nullptr;
        return result;
    }

    [[nodiscard]] std::uint64_t waiting() const noexcept { return waiting_; }

 private:
    static gen::FlowConfig flow_config(const Options& opt) {
        gen::FlowConfig cfg = opt.flow;
        cfg.owners = opt.connections;  // one owner per connection
        return cfg;
    }

    // The flow numbers owners from 1. Its deliberately wrong requests can name
    // an owner one past the last; that wraps round to the first connection,
    // which is just as wrong an owner.
    [[nodiscard]] std::size_t connection_of(engine::OwnerId owner) const noexcept {
        return (owner + connections_.size() - 1) % connections_.size();
    }

    template <class M>
    static void append(std::vector<std::byte>& out, const M& message) {
        const std::size_t at = out.size();
        out.resize(at + feed::wire_size(message));
        feed::encode(message, out.data() + at);
    }

    void send_one(std::uint64_t due, std::uint64_t now) {
        const gen::Command c = flow_.next();
        const engine::OwnerId owner = c.kind == gen::CommandKind::New ? c.order.owner : c.owner;
        Connection& conn = connections_[connection_of(owner)];
        switch (c.kind) {
            case gen::CommandKind::New:
                append(conn.out, net::EnterOrder{.token = c.order.token,
                                                 .locate = c.order.locate,
                                                 .side = c.order.side,
                                                 .qty = c.order.qty,
                                                 .price = c.order.price,
                                                 .kind = net::to_wire(c.order.kind),
                                                 .tif = net::to_wire(c.order.tif)});
                break;
            case gen::CommandKind::Cancel:
                append(conn.out, net::CancelOrder{.order_id = c.target});
                break;
            case gen::CommandKind::Replace:
                append(conn.out,
                       net::ReplaceOrder{.order_id = c.target, .qty = c.qty, .price = c.price});
                break;
        }
        conn.pending.sent(due);
        ++waiting_;
        if (due >= measure_from_) {
            ++result_->sent;
            result_->worst_lag_ns = std::max(result_->worst_lag_ns, now - due);
        }
    }

    // Writes what the sockets will take. Returns whether anything was written.
    bool write_pending() {
        bool wrote = false;
        for (Connection& conn : connections_) {
            while (conn.out_sent < conn.out.size()) {
                const ssize_t n = ::send(conn.fd.get(), conn.out.data() + conn.out_sent,
                                         conn.out.size() - conn.out_sent, MSG_NOSIGNAL);
                if (n > 0) {
                    conn.out_sent += static_cast<std::size_t>(n);
                    wrote = true;
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;  // the server is not reading; try again next time round
                }
                throw std::runtime_error(net::errno_text("the gateway closed a connection"));
            }
            if (conn.out_sent == conn.out.size()) {
                conn.out.clear();
                conn.out_sent = 0;
            }
        }
        return wrote;
    }

    // Reads every acknowledgement that has arrived. Returns whether any did.
    bool read_ready() {
        const std::span<const epoll_event> events = loop_.wait(0);
        for (const epoll_event& ev : events) {
            const auto index = static_cast<std::size_t>(ev.data.u64);
            Connection& conn = connections_[index];
            const ssize_t n = ::recv(conn.fd.get(), scratch_.data(), scratch_.size(), 0);
            // The clock is read once per read, right after it: every
            // acknowledgement in this batch became visible to us now.
            const std::uint64_t arrived = now_ns();
            if (n == 0) {
                throw std::runtime_error("the gateway closed a connection");
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(net::errno_text("reading from the gateway"));
            }
            conn.in.insert(conn.in.end(), scratch_.begin(), scratch_.begin() + n);
            std::size_t at = 0;
            while (at < conn.in.size()) {
                const std::size_t size = net::response_size(static_cast<char>(conn.in[at]));
                if (size == 0) {
                    throw std::runtime_error("the gateway sent a message of an unknown type");
                }
                if (conn.in.size() - at < size) {
                    break;
                }
                on_response(conn, static_cast<engine::OwnerId>(index + 1), conn.in.data() + at,
                            arrived);
                at += size;
            }
            conn.in.erase(conn.in.begin(), conn.in.begin() + static_cast<std::ptrdiff_t>(at));
        }
        return !events.empty();
    }

    // The request at the front of the connection's queue has been answered.
    void acknowledged(Connection& conn, std::uint64_t arrived, bool rejected) {
        if (conn.pending.empty()) {
            throw std::runtime_error("the gateway acknowledged a request that was never sent");
        }
        const bool measured = conn.pending.oldest() >= measure_from_;
        const std::uint64_t took = conn.pending.acknowledged(arrived);
        --waiting_;
        if (measured) {
            ++result_->acknowledged;
            result_->rejected += rejected ? 1U : 0U;
            result_->latency.record(took);
        }
    }

    void on_response(Connection& conn, engine::OwnerId owner, const std::byte* message,
                     std::uint64_t arrived) {
        switch (static_cast<char>(message[0])) {
            case net::OrderAccepted::kType: {
                const auto m = feed::decode<net::OrderAccepted>(message);
                flow_.on_accepted(engine::Accepted{.order_id = m.order_id,
                                                   .owner = owner,
                                                   .token = m.token,
                                                   .locate = m.locate,
                                                   .side = m.side,
                                                   .qty = m.qty,
                                                   .price = m.price});
                acknowledged(conn, arrived, false);
                break;
            }
            case net::OrderExecuted::kType: {
                const auto m = feed::decode<net::OrderExecuted>(message);
                flow_.on_executed(engine::Executed{.order_id = m.order_id,
                                                   .owner = owner,
                                                   .token = m.token,
                                                   .qty = m.qty,
                                                   .price = m.price,
                                                   .leaves = m.leaves});
                ++result_->executions;
                break;  // not an acknowledgement: nobody asked for a fill
            }
            case net::OrderCancelled::kType: {
                const auto m = feed::decode<net::OrderCancelled>(message);
                flow_.on_cancelled(engine::Cancelled{
                    .order_id = m.order_id, .owner = owner, .token = m.token, .qty = m.qty});
                // Only a cancel the owner asked for answers a request. The
                // other reasons follow an order that was already accepted.
                if (m.reason == 'U') {
                    acknowledged(conn, arrived, false);
                }
                break;
            }
            case net::OrderReplaced::kType: {
                const auto m = feed::decode<net::OrderReplaced>(message);
                flow_.on_replaced(engine::Replaced{.old_id = m.old_order_id,
                                                   .new_id = m.new_order_id,
                                                   .owner = owner,
                                                   .token = m.token,
                                                   .qty = m.qty,
                                                   .price = m.price,
                                                   .kept_priority = m.kept_priority == 'Y'});
                acknowledged(conn, arrived, false);
                break;
            }
            case net::OrderRejected::kType:
                acknowledged(conn, arrived, true);
                break;
            default:
                break;
        }
    }

    Options opt_;
    gen::OrderFlow flow_;
    net::EpollLoop loop_;
    std::vector<Connection> connections_;
    std::vector<std::byte> scratch_;
    RateResult* result_ = nullptr;
    std::uint64_t measure_from_ = 0;
    std::uint64_t waiting_ = 0;  // requests sent and not yet acknowledged
};

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------

[[nodiscard]] double micros(std::uint64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

void print_text(const Options& opt, const std::vector<std::unique_ptr<RateResult>>& results,
                bool pinned) {
    std::printf("load_gen%s%s\n", opt.label.empty() ? "" : "  label: ", opt.label.c_str());
    std::printf("  gateway     %s:%u over %u connections\n", opt.connect.host.c_str(),
                static_cast<unsigned>(opt.connect.port), opt.connections);
    std::printf("  flow        seed %" PRIu64 ", %u symbols, about %s resting orders\n",
                opt.flow.seed, opt.flow.symbols,
                util::with_commas(opt.flow.target_live_orders).c_str());
    std::printf("  each rate   %" PRIu64 " s measured after %" PRIu64 " s warm-up\n", opt.seconds,
                opt.warmup);
    std::printf("  cpu         %s\n", util::read_proc_value("/proc/cpuinfo", "model name").c_str());
    if (opt.cpu >= 0) {
        std::printf("  thread      %s CPU %d\n", pinned ? "pinned to" : "could NOT be pinned to",
                    opt.cpu);
    } else {
        std::printf("  thread      NOT pinned\n");
    }
    std::printf(
        "  latency     from the time a request was due to the time its\n"
        "              acknowledgement was read, in microseconds\n");

    std::printf("\n  %10s %10s %10s %9s %9s %9s %9s %10s %10s\n", "target/s", "sent", "acked",
                "p50", "p90", "p99", "p99.9", "max", "lag max");
    for (const auto& r : results) {
        std::printf("  %10s %10s %10s %9.1f %9.1f %9.1f %9.1f %10.1f %10.1f\n",
                    util::with_commas(r->target_rate).c_str(), util::with_commas(r->sent).c_str(),
                    util::with_commas(r->acknowledged).c_str(), micros(r->latency.percentile(50)),
                    micros(r->latency.percentile(90)), micros(r->latency.percentile(99)),
                    micros(r->latency.percentile(99.9)), micros(r->latency.max()),
                    micros(r->worst_lag_ns));
    }
    std::printf(
        "\n  every request that fell due was sent, however late: a generator that\n"
        "  cannot keep up shows as lag and as latency, never as a lower rate.\n");
    std::printf(
        "  \"lag max\" is how far behind its own schedule the generator fell at\n"
        "  worst. If it is not small next to the latencies, the generator was the\n"
        "  bottleneck and the row does not describe the server.\n");
    std::printf("  loopback only: this is the software path, not a network.\n");
    for (const auto& r : results) {
        if (r->unanswered != 0) {
            std::printf("\n  WARNING: at %s/s, %s requests were never acknowledged\n",
                        util::with_commas(r->target_rate).c_str(),
                        util::with_commas(r->unanswered).c_str());
        }
    }
}

bool write_json(const Options& opt, const std::vector<std::unique_ptr<RateResult>>& results) {
    std::FILE* f = std::fopen(opt.json_path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "{\n  \"benchmark\": \"load_gen\",\n");
    std::fprintf(f, "  \"connections\": %u,\n", opt.connections);
    std::fprintf(f, "  \"seconds\": %" PRIu64 ",\n", opt.seconds);
    std::fprintf(f, "  \"warmup_seconds\": %" PRIu64 ",\n", opt.warmup);
    std::fprintf(f, "  \"symbols\": %u,\n", opt.flow.symbols);
    std::fprintf(f, "  \"seed\": %" PRIu64 ",\n", opt.flow.seed);
    std::fprintf(f, "  \"loopback\": true,\n");
    std::fprintf(f, "  \"rates\": [\n");
    for (std::size_t i = 0; i < results.size(); ++i) {
        const RateResult& r = *results[i];
        std::fprintf(
            f,
            "    {\"target_rate\": %" PRIu64 ", \"achieved_rate\": %.1f, \"sent\": %" PRIu64
            ", \"acknowledged\": %" PRIu64 ", \"rejected\": %" PRIu64 ", \"executions\": %" PRIu64
            ", \"unanswered\": %" PRIu64
            // Three decimals of a microsecond is a whole nanosecond: nothing is
            // rounded away that a reader of the file would round differently.
            ", \"p50_us\": %.3f, \"p90_us\": %.3f, \"p99_us\": %.3f, "
            "\"p999_us\": %.3f, \"max_us\": %.3f, \"mean_us\": %.3f, "
            "\"worst_lag_us\": %.3f}%s\n",
            r.target_rate, r.achieved_rate(), r.sent, r.acknowledged, r.rejected, r.executions,
            r.unanswered, micros(r.latency.percentile(50)), micros(r.latency.percentile(90)),
            micros(r.latency.percentile(99)), micros(r.latency.percentile(99.9)),
            micros(r.latency.max()), r.latency.mean() / 1000.0, micros(r.worst_lag_ns),
            i + 1 == results.size() ? "" : ",");
    }
    std::fprintf(f, "  ]\n}\n");
    return std::fclose(f) == 0;
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
        if (arg == "--connect" && has_next) {
            if (!net::parse_endpoint(argv[++i], opt.connect)) {
                std::fprintf(stderr, "error: --connect needs HOST:PORT with an IPv4 address\n");
                return 1;
            }
        } else if (arg == "--rate" && has_next) {
            if (!parse_rates(argv[++i], opt.rates)) {
                std::fprintf(stderr, "error: --rate needs positive numbers, as in 1000,10000\n");
                return 1;
            }
        } else if (arg == "--json" && has_next) {
            opt.json_path = argv[++i];
        } else if (arg == "--label" && has_next) {
            opt.label = argv[++i];
        } else if (arg == "--connections" && has_value) {
            opt.connections = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--seconds" && has_value) {
            opt.seconds = value;
            ++i;
        } else if (arg == "--warmup" && has_value) {
            opt.warmup = value;
            ++i;
        } else if (arg == "--symbols" && has_value) {
            opt.flow.symbols = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--live" && has_value) {
            opt.flow.target_live_orders = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--seed" && has_value) {
            opt.flow.seed = value;
            ++i;
        } else if (arg == "--cpu" && has_value) {
            opt.cpu = static_cast<int>(value);
            ++i;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (opt.connections == 0 || opt.connections > 10'000 || opt.seconds == 0) {
        std::fprintf(stderr, "error: --connections must be 1 to 10000 and --seconds at least 1\n");
        return 1;
    }

    const bool pinned = opt.cpu >= 0 && util::pin_to_cpu(opt.cpu);
    LoadGenerator generator(opt);
    std::vector<std::unique_ptr<RateResult>> results;
    results.reserve(opt.rates.size());
    for (const std::uint64_t rate : opt.rates) {
        results.push_back(generator.run(rate));
    }

    print_text(opt, results, pinned);
    if (!opt.json_path.empty() && !write_json(opt, results)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", opt.json_path.c_str());
        return 1;
    }
    for (const auto& r : results) {
        if (r->unanswered != 0) {
            return 2;
        }
    }
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
