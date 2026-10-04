// engine_bench: time a matching engine on a recorded tape of requests.
//
//   engine_bench [options]
//     --engine NAME      which engine (default: reference); `flow_gen --list`
//                        prints the names
//     --seed N           seed of the order flow (default 1)
//     --commands N       requests on the tape (default 2000000)
//     --symbols N        securities (default 100)
//     --owners N         participants (default 16)
//     --live N           resting orders the flow hovers around (default 20000)
//     --sinks null|feed  what receives the engine's output (default: null)
//                          null  nothing: matching and book-keeping only
//                          feed  every market-data message is encoded to bytes
//     --runs N           measured runs (default 5)
//     --warmup N         unmeasured runs before them (default 1)
//     --cpu N            pin the thread to CPU N (default: not pinned)
//     --clock tsc|steady per-request clock (default: tsc where available)
//     --no-verify        skip the comparison with the reference engine
//     --json FILE        also write the results as JSON
//     --label TEXT       free text carried into the report
//
// How it works, and why it is built this way:
//
//   1. The requests are generated once, by running the seeded flow through the
//      reference engine, and kept as a tape. Generating them is not part of
//      any timing: the generator's own cost would otherwise sit inside every
//      number, and it is larger than the engine's.
//   2. The tape is replayed through the chosen engine with hashing sinks. Its
//      market data and reports must hash to the reference's, or the timings
//      are void (exit status 3). A fast engine that is wrong is not fast.
//   3. Each run replays the tape into a fresh engine. The requests up to the
//      point where the book first reached its target size are applied before
//      the clock starts, so what is timed is a full book in steady state, not
//      an empty one filling up.
//
// Like replay_bench, each run makes two passes: one with a single clock read
// before and after (throughput), one with a clock read per request (latency).
// docs/benchmark-method.md applies; read it before quoting a number.
//
// Exit status: 0 ok, 1 usage error, 3 the engine disagreed with the reference,
// 4 the chosen engine is not written yet.

#include <array>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "bench_env.hpp"
#include "obe/engine/engines.hpp"
#include "obe/engine/feed_writer.hpp"
#include "obe/engine/output_hash.hpp"
#include "obe/feed/codec.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/util/clock.hpp"
#include "obe/util/cpu.hpp"
#include "obe/util/format.hpp"
#include "obe/util/huge_pages.hpp"
#include "obe/util/latency_histogram.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;
using bench::summarize;
using bench::Summary;
using util::LatencyHistogram;

struct Options {
    gen::FlowConfig flow{.symbols = 100, .owners = 16, .target_live_orders = 20'000};
    std::uint64_t commands = 2'000'000;
    std::string engine = "reference";
    std::string sinks = "null";
    std::string clock = OBE_HAS_TSC ? "tsc" : "steady";
    std::string json_path;
    std::string label;
    int runs = 5;
    int warmup = 1;
    int cpu = -1;
    bool verify = true;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: engine_bench [--engine NAME] [--seed N] [--commands N] [--symbols N] "
                 "[--owners N]\n"
                 "                    [--live N] [--sinks null|feed] [--runs N] [--warmup N] "
                 "[--cpu N]\n"
                 "                    [--clock tsc|steady] [--no-verify] [--json FILE] "
                 "[--label TEXT]\n");
    return status;
}

template <class T>
bool parse_number(std::string_view text, T& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

// ---------------------------------------------------------------------------
// The tape
// ---------------------------------------------------------------------------

struct Hashes {
    std::uint64_t feed = 0;
    std::uint64_t reports = 0;

    friend bool operator==(const Hashes&, const Hashes&) = default;
};

struct Tape {
    std::vector<gen::Command> commands;
    // Requests before this index build the book up to its target size.
    std::size_t prefill = 0;
    engine::EngineStats stats;  // what the whole tape does, by the reference
    std::uint64_t market_data_messages = 0;
    Hashes reference;
};

// Folds market data into a hash without keeping the bytes.
class HashingFeed {
 public:
    template <class M>
    void take(const M& m) {
        std::array<std::byte, feed::kMaxMessageSize> bytes{};
        const std::size_t size = feed::encode(m, bytes.data());
        hash_.update({bytes.data(), size});
        ++messages_;
    }
    void on_stock_directory(const feed::StockDirectory& m) { take(m); }
    void on_trading_action(const feed::TradingAction& m) { take(m); }
    void on_add(const feed::AddOrder& m) { take(m); }
    void on_execute(const feed::OrderExecuted& m) { take(m); }
    void on_cancel(const feed::OrderCancel& m) { take(m); }
    void on_delete(const feed::OrderDelete& m) { take(m); }
    void on_replace(const feed::OrderReplace& m) { take(m); }

    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_.hash(); }
    [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }

 private:
    engine::ByteHasher hash_;
    std::uint64_t messages_ = 0;
};

Tape record_tape(const Options& opt) {
    using Reports = engine::TeeReports<gen::OrderFlow, engine::ReportHasher>;
    using Engine = engine::ReferenceEngine<Reports, HashingFeed>;

    Tape tape;
    tape.commands.reserve(opt.commands);
    gen::OrderFlow flow(opt.flow);
    engine::ReportHasher report_hash;
    Reports reports(flow, report_hash);
    HashingFeed feed_hash;
    const auto engine = std::make_unique<Engine>(reports, feed_hash);
    flow.open(*engine);

    bool filled = false;
    for (std::uint64_t i = 0; i < opt.commands; ++i) {
        const gen::Command c = flow.next();
        gen::apply(*engine, c);
        tape.commands.push_back(c);
        if (!filled && flow.live() >= flow.config().target_live_orders) {
            filled = true;
            tape.prefill = tape.commands.size();
        }
    }
    tape.stats = engine->stats();
    tape.market_data_messages = feed_hash.messages();
    tape.reference = {feed_hash.hash(), report_hash.hash()};
    return tape;
}

// Opens the tape's instruments on an engine, as the flow did when recording.
template <class Engine>
void open_instruments(Engine& engine, const Options& opt) {
    gen::OrderFlow flow(opt.flow);
    flow.open(engine);
}

// The tape through an engine with hashing sinks: what that engine says.
template <class Impl>
Hashes hash_engine(const Options& opt, const Tape& tape) {
    using Engine = typename Impl::template Engine<engine::ReportHasher, HashingFeed>;
    engine::ReportHasher report_hash;
    HashingFeed feed_hash;
    const auto engine = std::make_unique<Engine>(report_hash, feed_hash);
    open_instruments(*engine, opt);
    for (const gen::Command& c : tape.commands) {
        gen::apply(*engine, c);
    }
    return {feed_hash.hash(), report_hash.hash()};
}

// ---------------------------------------------------------------------------
// Sinks for the timed runs
// ---------------------------------------------------------------------------

// Encodes every market-data message, as a publisher would, and keeps a running
// checksum of the bytes so the work cannot be optimized away.
class EncodingFeed {
 public:
    template <class M>
    void take(const M& m) noexcept {
        std::array<std::byte, feed::kMaxMessageSize> bytes{};
        const std::size_t size = feed::encode(m, bytes.data());
        for (std::size_t i = 0; i < size; ++i) {
            sum_ += static_cast<std::uint64_t>(bytes[i]);
        }
    }
    void on_stock_directory(const feed::StockDirectory& m) noexcept { take(m); }
    void on_trading_action(const feed::TradingAction& m) noexcept { take(m); }
    void on_add(const feed::AddOrder& m) noexcept { take(m); }
    void on_execute(const feed::OrderExecuted& m) noexcept { take(m); }
    void on_cancel(const feed::OrderCancel& m) noexcept { take(m); }
    void on_delete(const feed::OrderDelete& m) noexcept { take(m); }
    void on_replace(const feed::OrderReplace& m) noexcept { take(m); }

    [[nodiscard]] std::uint64_t sum() const noexcept { return sum_; }

 private:
    std::uint64_t sum_ = 0;
};

// ---------------------------------------------------------------------------
// The timed passes
// ---------------------------------------------------------------------------

struct Run {
    std::uint64_t commands = 0;
    double seconds = 0;
    double cmds_per_s = 0;
    double mean_ns = 0;
    double p50_ns = 0;
    double p99_ns = 0;
    double p999_ns = 0;
    double max_ns = 0;
    std::uint64_t digest = 0;  // of the engine's final state; must not vary
};

// What an engine ends a run with, folded into one number.
template <class Engine>
std::uint64_t digest_of(const Engine& engine) {
    const engine::EngineStats& s = engine.stats();
    std::uint64_t h = engine::detail::kHashSeed;
    for (const std::uint64_t word :
         {s.accepted, s.rejected, s.cancels, s.replaces, s.trades, s.traded_shares,
          s.unfilled_shares, static_cast<std::uint64_t>(engine.open_orders())}) {
        h = engine::detail::mix(h, word);
    }
    return h;
}

template <class Impl, class Feed, class Clock>
Run one_run(const Options& opt, const Tape& tape) {
    using Engine = typename Impl::template Engine<engine::NullReports, Feed>;
    const gen::Command* const first = tape.commands.data() + tape.prefill;
    const gen::Command* const last = tape.commands.data() + tape.commands.size();
    Run run;
    run.commands = static_cast<std::uint64_t>(last - first);

    // Pass 1: throughput. No clock inside the loop.
    {
        engine::NullReports reports;
        Feed market_data;
        const auto engine = std::make_unique<Engine>(reports, market_data);
        open_instruments(*engine, opt);
        for (const gen::Command* c = tape.commands.data(); c != first; ++c) {
            gen::apply(*engine, *c);
        }
        const auto start = std::chrono::steady_clock::now();
        // ---- timed region starts ----
        for (const gen::Command* c = first; c != last; ++c) {
            gen::apply(*engine, *c);
        }
        // ---- timed region ends ----
        const auto end = std::chrono::steady_clock::now();
        run.seconds = std::chrono::duration<double>(end - start).count();
        run.digest = digest_of(*engine);
    }

    // Pass 2: latency. One clock read per request; a sample runs from the
    // previous read to this one.
    {
        engine::NullReports reports;
        Feed market_data;
        const auto engine = std::make_unique<Engine>(reports, market_data);
        open_instruments(*engine, opt);
        for (const gen::Command* c = tape.commands.data(); c != first; ++c) {
            gen::apply(*engine, *c);
        }
        const auto ticks = std::make_unique<LatencyHistogram>();
        std::uint64_t previous = Clock::now();
        for (const gen::Command* c = first; c != last; ++c) {
            gen::apply(*engine, *c);
            const std::uint64_t now = Clock::now();
            ticks->record(now - previous);
            previous = now;
        }
        const double ns = Clock::ns_per_tick();
        run.p50_ns = static_cast<double>(ticks->percentile(50)) * ns;
        run.p99_ns = static_cast<double>(ticks->percentile(99)) * ns;
        run.p999_ns = static_cast<double>(ticks->percentile(99.9)) * ns;
        run.max_ns = static_cast<double>(ticks->max()) * ns;
    }

    if (run.seconds > 0 && run.commands > 0) {
        run.cmds_per_s = static_cast<double>(run.commands) / run.seconds;
        run.mean_ns = run.seconds * 1e9 / static_cast<double>(run.commands);
    }
    return run;
}

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------

struct Report {
    Options options;
    std::string engine_description;
    bench::Environment env;
    std::string clock_name;
    double ns_per_tick = 1.0;
    bool tsc_invariant = false;
    double floor_p50_ns = 0;
    double floor_p99_ns = 0;
    std::size_t huge_bytes = 0;
    Tape tape;  // commands are dropped before the report is kept; the rest stays
    std::uint64_t timed_commands = 0;
    Hashes hashes;
    bool compared = false;
    std::vector<Run> runs;
    std::vector<std::string> warnings;

    [[nodiscard]] bool matches_reference() const { return !compared || hashes == tape.reference; }
};

void print_text(const Report& r) {
    const Summary rate = summarize(r.runs, [](const Run& x) { return x.cmds_per_s; });
    const Summary mean = summarize(r.runs, [](const Run& x) { return x.mean_ns; });
    const Summary p50 = summarize(r.runs, [](const Run& x) { return x.p50_ns; });
    const Summary p99 = summarize(r.runs, [](const Run& x) { return x.p99_ns; });
    const Summary p999 = summarize(r.runs, [](const Run& x) { return x.p999_ns; });
    const Summary max = summarize(r.runs, [](const Run& x) { return x.max_ns; });
    const engine::EngineStats& s = r.tape.stats;

    std::printf("engine_bench%s%s\n",
                r.options.label.empty() ? "" : "  label: ", r.options.label.c_str());
    std::printf("  engine      %s: %s\n", r.options.engine.c_str(), r.engine_description.c_str());
    std::printf("  output      %s\n",
                r.options.sinks == "feed"
                    ? "feed: every market-data message encoded to bytes; reports dropped"
                    : "null: market data and reports dropped (matching and book-keeping only)");
    std::printf("  tape        seed %" PRIu64 ", %u symbols, %u owners, about %s resting orders\n",
                r.options.flow.seed, r.options.flow.symbols, r.options.flow.owners,
                util::with_commas(r.options.flow.target_live_orders).c_str());
    std::printf("              %s requests: %s to fill the book (not timed), %s timed\n",
                util::with_commas(r.options.commands).c_str(),
                util::with_commas(r.tape.prefill).c_str(),
                util::with_commas(r.timed_commands).c_str());
    std::printf(
        "              whole tape: %s orders accepted, %s cancels, %s replaces, %s "
        "trades, %s rejects,\n",
        util::with_commas(s.accepted).c_str(), util::with_commas(s.cancels).c_str(),
        util::with_commas(s.replaces).c_str(), util::with_commas(s.trades).c_str(),
        util::with_commas(s.rejected).c_str());
    std::printf("              %s market-data messages\n",
                util::with_commas(r.tape.market_data_messages).c_str());
    std::printf("  runs        %d measured after %d warm-up\n", r.options.runs, r.options.warmup);
    bench::print_environment(r.env, r.huge_bytes);
    std::printf("  clock       %s, %.4f ns per tick%s\n", r.clock_name.c_str(), r.ns_per_tick,
                r.clock_name == "rdtsc"
                    ? (r.tsc_invariant ? ", invariant TSC" : ", TSC NOT invariant")
                    : "");
    std::printf(
        "  timer cost  p50 %.1f ns, p99 %.1f ns per sample (included in the latencies "
        "below)\n",
        r.floor_p50_ns, r.floor_p99_ns);
    if (r.compared) {
        std::printf("  check       feed hash %016" PRIx64 ", report hash %016" PRIx64 "\n",
                    r.hashes.feed, r.hashes.reports);
        if (r.matches_reference()) {
            std::printf("              identical to the reference engine's output\n");
        } else {
            std::printf("              DIFFERS FROM THE REFERENCE (%016" PRIx64 ", %016" PRIx64
                        "): THE TIMINGS BELOW ARE VOID\n",
                        r.tape.reference.feed, r.tape.reference.reports);
        }
    } else {
        std::printf("  check       not compared with the reference (--no-verify)\n");
    }

    std::printf("\n  %-6s %12s %9s %9s %9s %9s %11s\n", "run", "requests/s", "mean ns", "p50 ns",
                "p99 ns", "p99.9 ns", "max ns");
    for (std::size_t i = 0; i < r.runs.size(); ++i) {
        const Run& x = r.runs[i];
        std::printf("  %-6zu %12.0f %9.1f %9.1f %9.1f %9.1f %11.0f\n", i + 1, x.cmds_per_s,
                    x.mean_ns, x.p50_ns, x.p99_ns, x.p999_ns, x.max_ns);
    }
    std::printf("  %-6s %12.0f %9.1f %9.1f %9.1f %9.1f %11.0f\n", "median", rate.median,
                mean.median, p50.median, p99.median, p999.median, max.median);
    std::printf("  %-6s %12.0f %9.1f %9.1f %9.1f %9.1f %11.0f\n", "min", rate.min, mean.min,
                p50.min, p99.min, p999.min, max.min);
    std::printf("  %-6s %12.0f %9.1f %9.1f %9.1f %9.1f %11.0f\n", "max", rate.max, mean.max,
                p50.max, p99.max, p999.max, max.max);
    std::printf("\n  throughput spread across runs: %.1f%% of the median\n", rate.spread_percent());
    std::printf("  requests/s and mean ns come from the pass with no per-request clock;\n");
    std::printf("  percentiles come from a second pass with one clock read per request.\n");
    std::printf("  the flow is synthetic: these numbers compare engines, they do not describe\n");
    std::printf("  an exchange.\n");

    for (const std::string& w : r.warnings) {
        std::printf("\n  WARNING: %s\n", w.c_str());
    }
}

bool write_json(const Report& r) {
    std::FILE* f = std::fopen(r.options.json_path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    const Summary rate = summarize(r.runs, [](const Run& x) { return x.cmds_per_s; });
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"benchmark\": \"engine_bench\",\n");
    std::fprintf(f, "  \"label\": \"%s\",\n", bench::json_escape(r.options.label).c_str());
    std::fprintf(f, "  \"workload\": \"engine/%s/%s\",\n",
                 bench::json_escape(r.options.engine).c_str(),
                 bench::json_escape(r.options.sinks).c_str());
    std::fprintf(f, "  \"seed\": %" PRIu64 ",\n", r.options.flow.seed);
    std::fprintf(f, "  \"symbols\": %u,\n", r.options.flow.symbols);
    std::fprintf(f, "  \"owners\": %u,\n", r.options.flow.owners);
    std::fprintf(f, "  \"target_live_orders\": %u,\n", r.options.flow.target_live_orders);
    std::fprintf(f, "  \"requests\": %" PRIu64 ",\n", r.options.commands);
    std::fprintf(f, "  \"prefill_requests\": %zu,\n", r.tape.prefill);
    std::fprintf(f, "  \"messages\": %" PRIu64 ",\n", r.timed_commands);
    std::fprintf(f, "  \"trades\": %" PRIu64 ",\n", r.tape.stats.trades);
    std::fprintf(f, "  \"market_data_messages\": %" PRIu64 ",\n", r.tape.market_data_messages);
    std::fprintf(f, "  \"warmup_runs\": %d,\n", r.options.warmup);
    bench::write_environment_json(f, r.env, r.huge_bytes);
    std::fprintf(f, "  \"clock\": \"%s\",\n", bench::json_escape(r.clock_name).c_str());
    std::fprintf(f, "  \"ns_per_tick\": %.6f,\n", r.ns_per_tick);
    std::fprintf(f, "  \"tsc_invariant\": %s,\n", r.tsc_invariant ? "true" : "false");
    std::fprintf(f, "  \"timer_floor_p50_ns\": %.2f,\n", r.floor_p50_ns);
    std::fprintf(f, "  \"timer_floor_p99_ns\": %.2f,\n", r.floor_p99_ns);
    std::fprintf(f, "  \"feed_hash\": \"%016" PRIx64 "\",\n", r.hashes.feed);
    std::fprintf(f, "  \"report_hash\": \"%016" PRIx64 "\",\n", r.hashes.reports);
    std::fprintf(f, "  \"compared_with_reference\": %s,\n", r.compared ? "true" : "false");
    std::fprintf(f, "  \"matches_reference\": %s,\n", r.matches_reference() ? "true" : "false");
    std::fprintf(f, "  \"median_msgs_per_s\": %.0f,\n", rate.median);
    std::fprintf(f, "  \"min_msgs_per_s\": %.0f,\n", rate.min);
    std::fprintf(f, "  \"max_msgs_per_s\": %.0f,\n", rate.max);
    std::fprintf(f, "  \"runs\": [\n");
    for (std::size_t i = 0; i < r.runs.size(); ++i) {
        const Run& x = r.runs[i];
        std::fprintf(f,
                     "    {\"msgs_per_s\": %.0f, \"mean_ns\": %.2f, \"p50_ns\": %.2f, "
                     "\"p99_ns\": %.2f, \"p999_ns\": %.2f, \"max_ns\": %.0f}%s\n",
                     x.cmds_per_s, x.mean_ns, x.p50_ns, x.p99_ns, x.p999_ns, x.max_ns,
                     i + 1 == r.runs.size() ? "" : ",");
    }
    std::fprintf(f, "  ],\n");
    std::fprintf(f, "  \"warnings\": [");
    for (std::size_t i = 0; i < r.warnings.size(); ++i) {
        std::fprintf(f, "%s\"%s\"", i == 0 ? "" : ", ", bench::json_escape(r.warnings[i]).c_str());
    }
    std::fprintf(f, "]\n}\n");
    return std::fclose(f) == 0;
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

template <class Impl, class Feed, class Clock>
int measure(const Options& opt, Report& report, const Tape& tape) {
    report.clock_name = Clock::kName;
    report.ns_per_tick = Clock::ns_per_tick();
    {
        const auto floor = bench::timer_floor<Clock>(5'000'000);
        report.floor_p50_ns = static_cast<double>(floor->percentile(50)) * report.ns_per_tick;
        report.floor_p99_ns = static_cast<double>(floor->percentile(99)) * report.ns_per_tick;
    }

    if (opt.verify) {
        report.hashes = hash_engine<Impl>(opt, tape);
        report.compared = true;
    }

    for (int i = 0; i < opt.warmup; ++i) {
        static_cast<void>(one_run<Impl, Feed, Clock>(opt, tape));
    }
    for (int i = 0; i < opt.runs; ++i) {
        report.runs.push_back(one_run<Impl, Feed, Clock>(opt, tape));
        if (report.runs.back().digest != report.runs.front().digest) {
            report.warnings.emplace_back(
                "two runs ended in different engine states: the workload is not deterministic");
        }
    }
    report.huge_bytes = util::process_huge_bytes();

    const Summary rate = summarize(report.runs, [](const Run& x) { return x.cmds_per_s; });
    if (rate.spread_percent() > 5.0) {
        report.warnings.emplace_back(
            "throughput varied by more than 5% across runs: fix the environment before "
            "comparing two builds");
    }

    print_text(report);
    if (!opt.json_path.empty() && !write_json(report)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", opt.json_path.c_str());
        return 1;
    }
    return report.matches_reference() ? 0 : 3;
}

template <class Impl>
int bench_engine(const Options& opt, Report& report, const Tape& tape) {
    report.engine_description = std::string(Impl::kDescription);
    const bool tsc = opt.clock == "tsc";
    if (opt.sinks == "feed") {
        return tsc ? measure<Impl, EncodingFeed, util::TscClock>(opt, report, tape)
                   : measure<Impl, EncodingFeed, util::SteadyClock>(opt, report, tape);
    }
    return tsc ? measure<Impl, engine::NullMarketData, util::TscClock>(opt, report, tape)
               : measure<Impl, engine::NullMarketData, util::SteadyClock>(opt, report, tape);
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
        if (arg == "--no-verify") {
            opt.verify = false;
        } else if (arg == "--engine" && has_next) {
            opt.engine = argv[++i];
        } else if (arg == "--sinks" && has_next) {
            opt.sinks = argv[++i];
        } else if (arg == "--clock" && has_next) {
            opt.clock = argv[++i];
        } else if (arg == "--json" && has_next) {
            opt.json_path = argv[++i];
        } else if (arg == "--label" && has_next) {
            opt.label = argv[++i];
        } else if (arg == "--seed" && has_value) {
            opt.flow.seed = value;
            ++i;
        } else if (arg == "--commands" && has_value) {
            opt.commands = value;
            ++i;
        } else if (arg == "--symbols" && has_value) {
            opt.flow.symbols = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--owners" && has_value) {
            opt.flow.owners = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--live" && has_value) {
            opt.flow.target_live_orders = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--runs" && has_value) {
            opt.runs = static_cast<int>(value);
            ++i;
        } else if (arg == "--warmup" && has_value) {
            opt.warmup = static_cast<int>(value);
            ++i;
        } else if (arg == "--cpu" && has_value) {
            opt.cpu = static_cast<int>(value);
            ++i;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (opt.sinks != "null" && opt.sinks != "feed") {
        std::fprintf(stderr, "error: --sinks must be null or feed\n");
        return 1;
    }
    if (opt.clock != "tsc" && opt.clock != "steady") {
        std::fprintf(stderr, "error: --clock must be tsc or steady\n");
        return 1;
    }
    if (opt.runs < 1 || opt.commands == 0) {
        std::fprintf(stderr, "error: --runs and --commands must be at least 1\n");
        return 1;
    }

    Report report;
    report.options = opt;
    report.tsc_invariant = util::tsc_is_invariant();

    bool pinned = false;
    if (opt.cpu >= 0) {
        pinned = util::pin_to_cpu(opt.cpu);
        if (!pinned) {
            report.warnings.emplace_back("could not pin to CPU " + std::to_string(opt.cpu));
        }
    } else {
        report.warnings.emplace_back(
            "the thread is not pinned (--cpu N): expect migrations in the tail");
    }
    report.env = bench::gather_environment(opt.cpu, pinned);
    if (!bench::is_release_build() || bench::is_instrumented_build()) {
        report.warnings.emplace_back(
            "this is not an optimized, uninstrumented build: the timings mean nothing");
    }
    if (opt.runs < 5) {
        report.warnings.emplace_back("fewer than 5 measured runs: not enough to quote");
    }
    if (opt.clock == "tsc" && !report.tsc_invariant) {
        report.warnings.emplace_back(
            "the CPU does not advertise an invariant TSC: use --clock steady");
    }

    // The tape is recorded with the reference engine whichever engine is timed.
    Tape tape = record_tape(opt);
    report.timed_commands = tape.commands.size() - tape.prefill;
    if (report.timed_commands == 0) {
        std::fprintf(stderr,
                     "error: the book never reached %u resting orders in %" PRIu64
                     " requests, so there is nothing to time. Raise --commands or lower --live.\n",
                     opt.flow.target_live_orders, opt.commands);
        return 1;
    }
    report.tape.prefill = tape.prefill;
    report.tape.stats = tape.stats;
    report.tape.market_data_messages = tape.market_data_messages;
    report.tape.reference = tape.reference;

    int status = 1;
    const bool known = engine::with_engine(opt.engine, [&]<class Impl>(std::type_identity<Impl>) {
        status = bench_engine<Impl>(opt, report, tape);
    });
    if (!known) {
        std::fprintf(stderr, "error: no engine called '%s'. Try flow_gen --list.\n",
                     opt.engine.c_str());
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
