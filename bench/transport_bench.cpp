// transport_bench: what one request costs in system calls, by transport.
//
//   transport_bench [options]
//     --io NAME          measure this transport only (default: every transport
//                        that is written and can run here)
//     --connections LIST connection counts to measure (default 1,8,64,256)
//     --requests N       requests per row, over all connections (default 200000)
//     --size N           bytes per request and per answer (default 32)
//     --runs N           measured runs per row (default 5)
//     --cpus S,C         pin the server thread to CPU S and the client thread
//                        to CPU C (default: not pinned)
//     --json FILE        also write the results as JSON
//     --label TEXT       free text carried into the report
//
// A server thread runs a transport with the smallest handler there is: it
// sends back whatever arrives. A client thread holds K connections and works
// in rounds: one request down every connection, then one answer read from
// every connection. So in each round the server finds K sockets with
// something to say at once, which is the situation the two ways of doing I/O
// differ in.
//
// For each transport and each K it reports
//
//   requests/s          over the whole exchange, as the client saw it
//   syscalls/request    the server transport's own count of its crossings
//                       into the kernel, divided by the requests it served
//
// How to read it
//
// The second column is the mechanism, the first is the consequence. A
// readiness transport pays about three system calls for a request that
// arrives alone (the wait, the read, the write) and a little over two each
// when many arrive together, because only the wait is shared. A completion
// transport can pay a fixed number per TURN, so its cost per request falls
// as 1/K. Whether that shows in requests per second depends on how much of a
// request's time was system-call overhead to begin with. Crossing into the
// kernel and back has a price that depends on the CPU and on which
// speculative-execution mitigations are on; what the kernel then does for a
// TCP packet on loopback usually costs more, and is the same for both
// transports. Expect the two columns to tell different stories, and say so
// when quoting either.
//
// This is a closed loop: the client sends the next round only when the last
// one is answered. It measures capacity and mechanism. It does not measure
// latency under load, and must not be quoted as if it did (docs/
// benchmark-method.md, on coordinated omission): the latency curve comes
// from exchange_server --io NAME and load_gen.
//
// Exit status: 0 ok, 1 usage or set-up error, 3 an answer was not the
// request it answered (the timings are void), 4 the transport named with --io
// is not written yet, 5 the transport named with --io cannot run on this
// system.

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "bench_env.hpp"
#include "obe/net/socket.hpp"
#include "obe/net/transport.hpp"
#include "obe/net/transports.hpp"
#include "obe/util/cpu.hpp"
#include "obe/util/format.hpp"
#include "obe/util/huge_pages.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;
using Clock = std::chrono::steady_clock;

struct Options {
    std::string io;  // empty: every transport that can be measured
    std::string json_path;
    std::string label;
    std::vector<std::uint32_t> connections = {1, 8, 64, 256};
    std::uint64_t requests = 200'000;
    std::size_t size = 32;
    int runs = 5;
    int server_cpu = -1;
    int client_cpu = -1;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: transport_bench [--io NAME] [--connections K1,K2,...] [--requests N] "
                 "[--size BYTES]\n"
                 "                       [--runs N] [--cpus S,C] [--json FILE] [--label TEXT]\n");
    return status;
}

// Sends back whatever arrives.
template <class Net>
struct Echo {
    Net* net = nullptr;
    net::ConnId next = 0;
    std::vector<net::ConnId> gone;

    net::ConnId on_connect() { return ++next; }
    void on_data(net::ConnId id, std::span<const std::byte> bytes) { net->send(id, bytes); }
    void on_disconnect(net::ConnId id, net::LinkState /*why*/) { gone.push_back(id); }
};

// What the server thread tells the client thread. The counts are published
// at the end of every turn, the request bytes last, so that a reader who has
// seen all its bytes counted sees a system-call count at least as new.
struct Shared {
    std::atomic<std::uint16_t> port{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<std::uint64_t> syscalls{0};
    std::atomic<std::uint64_t> bytes_in{0};
};

template <class Net>
void serve(Shared& shared, int cpu) {
    try {
        if (cpu >= 0) {
            static_cast<void>(util::pin_to_cpu(cpu));
        }
        Net net{net::TransportConfig{}};
        Echo<Net> echo;
        echo.net = &net;
        net.listen("127.0.0.1", 0);
        shared.port.store(net.port(), std::memory_order_release);
        while (!shared.stop.load(std::memory_order_acquire)) {
            net.poll(20, echo);
            net.flush_all(echo);
            for (const net::ConnId id : echo.gone) {
                net.close(id);
            }
            echo.gone.clear();
            const net::TransportStats stats = net.stats();
            shared.syscalls.store(stats.syscalls, std::memory_order_release);
            shared.bytes_in.store(stats.bytes_in, std::memory_order_release);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error in the server thread: %s\n", e.what());
        shared.failed.store(true, std::memory_order_release);
    }
}

// A blocking write or read of exactly `size` bytes. False if the connection
// ended first.
bool write_all(int fd, const std::byte* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::send(fd, data + done, size - done, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool read_all(int fd, std::byte* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::recv(fd, data + done, size - done, 0);
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

struct Row {
    std::string io;
    std::uint32_t connections = 0;
    std::uint64_t requests = 0;            // per run
    std::vector<double> requests_per_sec;  // one per run
    std::vector<double> syscalls_per_req;  // one per run
    bool ok = true;
};

struct Report {
    Options options;
    bench::Environment env;
    std::size_t huge_bytes = 0;
    std::vector<Row> rows;
    std::vector<std::string> left_out;
    std::vector<std::string> warnings;
};

// The rounds of one run: every connection sends request number `round`, then
// every connection reads it back. Returns false if an answer was wrong.
bool exchange(const std::vector<net::Fd>& sockets, std::uint64_t rounds, std::size_t size,
              std::uint64_t& counter) {
    std::vector<std::byte> out(size);
    std::vector<std::byte> in(size);
    for (std::uint64_t round = 0; round < rounds; ++round) {
        const std::uint64_t first = counter;
        for (const net::Fd& socket : sockets) {
            std::memcpy(out.data(), &counter, std::min(size, sizeof(counter)));
            ++counter;
            if (!write_all(socket.get(), out.data(), size)) {
                return false;
            }
        }
        std::uint64_t expected = first;
        for (const net::Fd& socket : sockets) {
            if (!read_all(socket.get(), in.data(), size)) {
                return false;
            }
            std::uint64_t got = 0;
            std::memcpy(&got, in.data(), std::min(size, sizeof(got)));
            std::uint64_t want = 0;
            std::memcpy(&want, &expected, std::min(size, sizeof(want)));
            if (got != want) {
                return false;
            }
            ++expected;
        }
    }
    return true;
}

// Stops the server thread and waits for it: when asked, and in any case
// when it goes out of scope.
struct StopAndJoin {
    Shared& shared;       // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::thread& thread;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)

    void now() const {
        shared.stop.store(true, std::memory_order_release);
        if (thread.joinable()) {
            thread.join();
        }
    }
    ~StopAndJoin() { now(); }
    StopAndJoin(Shared& s, std::thread& t) : shared(s), thread(t) {}
    StopAndJoin(const StopAndJoin&) = delete;
    StopAndJoin& operator=(const StopAndJoin&) = delete;
};

template <class Net>
Row measure(const Options& opt, std::uint32_t connections) {
    Row row;
    row.io = std::string(Net::kName);
    row.connections = connections;
    const std::uint64_t rounds = std::max<std::uint64_t>(1, opt.requests / connections);
    row.requests = rounds * connections;

    Shared shared;
    std::thread server([&] { serve<Net>(shared, opt.server_cpu); });
    // Whatever way this function is left, the server thread is told to stop
    // and waited for. A std::thread destroyed while still running ends the
    // whole program, and an exception below (a connection refused for want
    // of file descriptors, say) would otherwise do exactly that.
    const StopAndJoin stop_server{shared, server};
    const auto give_up = Clock::now() + std::chrono::seconds(10);
    while (shared.port.load(std::memory_order_acquire) == 0 &&
           !shared.failed.load(std::memory_order_acquire) && Clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::uint16_t port = shared.port.load(std::memory_order_acquire);
    if (port == 0) {
        throw std::runtime_error("the server thread did not start");
    }
    if (opt.client_cpu >= 0) {
        static_cast<void>(util::pin_to_cpu(opt.client_cpu));
    }

    std::vector<net::Fd> sockets;
    sockets.reserve(connections);
    for (std::uint32_t i = 0; i < connections; ++i) {
        sockets.push_back(net::connect_tcp("127.0.0.1", port, false));
    }

    // Waits for the server to have counted `bytes` in, and returns its count
    // of system calls from then.
    const auto syscalls_after = [&shared](std::uint64_t bytes) {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (shared.bytes_in.load(std::memory_order_acquire) < bytes && Clock::now() < deadline) {
            std::this_thread::yield();
        }
        return shared.syscalls.load(std::memory_order_acquire);
    };

    std::uint64_t counter = 1;
    std::uint64_t bytes = 0;
    for (int run = -1; run < opt.runs && row.ok; ++run) {  // run -1 is the warm-up
        const std::uint64_t syscalls_before = syscalls_after(bytes);
        const Clock::time_point start = Clock::now();
        row.ok = exchange(sockets, rounds, opt.size, counter);
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        bytes += row.requests * opt.size;
        const std::uint64_t syscalls = syscalls_after(bytes) - syscalls_before;
        if (run >= 0) {
            row.requests_per_sec.push_back(static_cast<double>(row.requests) / seconds);
            row.syscalls_per_req.push_back(static_cast<double>(syscalls) /
                                           static_cast<double>(row.requests));
        }
    }
    sockets.clear();
    stop_server.now();
    row.ok = row.ok && !shared.failed.load(std::memory_order_acquire);
    return row;
}

// Whether the transport's functions exist yet.
template <class Net>
bool written() {
    try {
        const Net probe{net::TransportConfig{}};
        return true;
    } catch (const util::Unimplemented&) {
        return false;
    }
}

bench::Summary summary(const std::vector<double>& runs) {
    return bench::summarize(runs, [](double v) { return v; });
}

void print_text(const Report& r) {
    const Options& opt = r.options;
    std::printf("transport_bench\n");
    std::printf(
        "  exchange    %zu-byte requests echoed, %s per row, one round on every "
        "connection at a time\n",
        opt.size, util::with_commas(opt.requests).c_str());
    std::printf("  runs        %d per row; the median is shown\n", opt.runs);
    if (!opt.label.empty()) {
        std::printf("  label       %s\n", opt.label.c_str());
    }
    bench::print_environment(r.env, r.huge_bytes);
    std::printf(
        "  threads     server %s, client %s\n",
        opt.server_cpu >= 0 ? ("on CPU " + std::to_string(opt.server_cpu)).c_str() : "not pinned",
        opt.client_cpu >= 0 ? ("on CPU " + std::to_string(opt.client_cpu)).c_str() : "not pinned");

    std::printf("\n  %-8s %12s %14s %8s %18s\n", "io", "connections", "requests/s", "spread",
                "syscalls/request");
    for (const Row& row : r.rows) {
        const bench::Summary rate = summary(row.requests_per_sec);
        std::printf("  %-8s %12u %14s %7.1f%% %18.2f%s\n", row.io.c_str(), row.connections,
                    util::with_commas(static_cast<std::uint64_t>(rate.median)).c_str(),
                    rate.spread_percent(), summary(row.syscalls_per_req).median,
                    row.ok ? "" : "   WRONG ANSWERS");
    }
    std::printf(
        "\n  The system calls are the server transport's own count. The client's are\n"
        "  not in it, and are the same whichever transport serves.\n");
    for (const std::string& name : r.left_out) {
        std::printf("\n  %s\n", name.c_str());
    }
    for (const std::string& w : r.warnings) {
        std::printf("\nWARNING: %s\n", w.c_str());
    }
}

bool write_json(const Report& r) {
    std::FILE* f = std::fopen(r.options.json_path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    const Options& opt = r.options;
    std::fprintf(f, "{\n  \"benchmark\": \"transport_bench\",\n");
    std::fprintf(f, "  \"label\": \"%s\",\n", bench::json_escape(opt.label).c_str());
    bench::write_environment_json(f, r.env, r.huge_bytes);
    std::fprintf(f, "  \"request_bytes\": %zu,\n", opt.size);
    std::fprintf(f, "  \"server_cpu\": %d,\n  \"client_cpu\": %d,\n", opt.server_cpu,
                 opt.client_cpu);
    std::fprintf(f, "  \"runs\": %d,\n", opt.runs);
    std::fprintf(f, "  \"rows\": [\n");
    for (std::size_t i = 0; i < r.rows.size(); ++i) {
        const Row& row = r.rows[i];
        const bench::Summary rate = summary(row.requests_per_sec);
        const bench::Summary calls = summary(row.syscalls_per_req);
        std::fprintf(f,
                     "    {\"io\": \"%s\", \"connections\": %u, \"requests\": %" PRIu64
                     ", \"requests_per_sec_median\": %.0f, \"requests_per_sec_min\": %.0f, "
                     "\"requests_per_sec_max\": %.0f, \"syscalls_per_request_median\": %.3f, "
                     "\"syscalls_per_request_min\": %.3f, \"syscalls_per_request_max\": %.3f, "
                     "\"ok\": %s}%s\n",
                     bench::json_escape(row.io).c_str(), row.connections, row.requests, rate.median,
                     rate.min, rate.max, calls.median, calls.min, calls.max,
                     row.ok ? "true" : "false", i + 1 < r.rows.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"left_out\": [");
    for (std::size_t i = 0; i < r.left_out.size(); ++i) {
        std::fprintf(f, "%s\"%s\"", i == 0 ? "" : ", ", bench::json_escape(r.left_out[i]).c_str());
    }
    std::fprintf(f, "],\n  \"warnings\": [");
    for (std::size_t i = 0; i < r.warnings.size(); ++i) {
        std::fprintf(f, "%s\"%s\"", i == 0 ? "" : ", ", bench::json_escape(r.warnings[i]).c_str());
    }
    std::fprintf(f, "]\n}\n");
    return std::fclose(f) == 0;
}

template <class T>
bool parse_number(std::string_view text, T& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

template <class T>
bool parse_list(std::string_view text, std::vector<T>& out) {
    out.clear();
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        T value{};
        if (!parse_number(text.substr(0, comma), value)) {
            return false;
        }
        out.push_back(value);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return !out.empty();
}

int run(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = i + 1 < argc;
        std::uint64_t value = 0;
        const bool has_value = has_next && parse_number(std::string_view(argv[i + 1]), value);
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--io" && has_next) {
            opt.io = argv[++i];
        } else if (arg == "--json" && has_next) {
            opt.json_path = argv[++i];
        } else if (arg == "--label" && has_next) {
            opt.label = argv[++i];
        } else if (arg == "--connections" && has_next) {
            if (!parse_list(argv[++i], opt.connections)) {
                std::fprintf(stderr, "error: --connections needs numbers, as in 1,8,64\n");
                return 1;
            }
        } else if (arg == "--cpus" && has_next) {
            std::vector<int> cpus;
            if (!parse_list(argv[++i], cpus) || cpus.size() != 2) {
                std::fprintf(stderr, "error: --cpus needs two CPU numbers, as in --cpus 2,4\n");
                return 1;
            }
            opt.server_cpu = cpus[0];
            opt.client_cpu = cpus[1];
        } else if (arg == "--requests" && has_value) {
            opt.requests = value;
            ++i;
        } else if (arg == "--size" && has_value) {
            opt.size = static_cast<std::size_t>(value);
            ++i;
        } else if (arg == "--runs" && has_value) {
            opt.runs = static_cast<int>(value);
            ++i;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    const bool bad_count = std::any_of(opt.connections.begin(), opt.connections.end(),
                                       [](std::uint32_t k) { return k == 0 || k > 4'000; });
    if (opt.runs < 1 || opt.requests == 0 || opt.size < 8 || opt.size > 65'536 || bad_count) {
        std::fprintf(stderr,
                     "error: --runs and --requests must be at least 1, --size from 8 to "
                     "65536, and each connection count from 1 to 4000\n");
        return 1;
    }
    if (!opt.io.empty() && !net::with_transport(opt.io, [](auto) {})) {
        std::fprintf(stderr, "error: no transport called '%s'. Try exchange_server --list-io.\n",
                     opt.io.c_str());
        return 1;
    }

    Report report;
    report.options = opt;
    report.env = bench::gather_environment(opt.client_cpu, opt.client_cpu >= 0);
    if (opt.server_cpu < 0 || opt.client_cpu < 0) {
        report.warnings.emplace_back(
            "the threads are not pinned (--cpus S,C): where the scheduler puts them changes "
            "the result");
    }
    if (report.env.logical_cpus < 2) {
        report.warnings.emplace_back("fewer than two CPUs: the two threads take turns on one");
    }
    if (!bench::is_release_build() || bench::is_instrumented_build()) {
        report.warnings.emplace_back(
            "this is not an optimized, uninstrumented build: the timings mean nothing");
    }
    if (opt.runs < 5) {
        report.warnings.emplace_back("fewer than 5 measured runs: not enough to quote");
    }

    bool ok = true;
    bool asked_for_unwritten = false;
    bool asked_for_unavailable = false;
    net::for_each_transport([&]<class Net>(std::type_identity<Net>) {
        if (!opt.io.empty() && opt.io != Net::kName) {
            return;
        }
        if (!Net::available()) {
            asked_for_unavailable = !opt.io.empty();
            report.left_out.emplace_back("'" + std::string(Net::kName) +
                                         "' cannot run on this system and was left out");
            return;
        }
        if (!written<Net>()) {
            asked_for_unwritten = !opt.io.empty();
            report.left_out.emplace_back("'" + std::string(Net::kName) +
                                         "' is not written yet and was left out");
            return;
        }
        for (const std::uint32_t connections : opt.connections) {
            Row row = measure<Net>(opt, connections);
            ok = ok && row.ok;
            if (summary(row.requests_per_sec).spread_percent() > 10.0) {
                report.warnings.emplace_back("'" + row.io + "' with " +
                                             std::to_string(connections) +
                                             " connections varied by more than 10% across runs");
            }
            report.rows.push_back(std::move(row));
        }
    });
    report.huge_bytes = util::process_huge_bytes();

    if (asked_for_unwritten) {
        net::with_transport(opt.io, []<class Net>(std::type_identity<Net>) {
            const Net probe{net::TransportConfig{}};  // throws, naming what is missing
        });
        return 4;
    }
    if (asked_for_unavailable) {
        std::fprintf(stderr,
                     "error: the '%s' transport cannot run here: the kernel is too old for it, "
                     "or this system does not allow it\n",
                     opt.io.c_str());
        return 5;
    }
    print_text(report);
    if (!opt.json_path.empty() && !write_json(report)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", opt.json_path.c_str());
        return 1;
    }
    if (report.rows.empty()) {
        std::fprintf(stderr, "error: there was nothing to measure\n");
        return 1;
    }
    if (!ok) {
        std::printf("\nRESULT: an answer was not the request it answered\n");
        return 3;
    }
    return 0;
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
