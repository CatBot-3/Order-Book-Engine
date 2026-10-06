// journal_bench: what recording every request costs, and what recovery costs.
//
//   journal_bench [options]
//     --dir DIR          where the journal file is written (default: .)
//     --engine NAME      which engine (default: reference)
//     --seed N           seed of the order flow (default 1)
//     --commands N       requests on the tape (default 1000000)
//     --symbols N        securities (default 100)
//     --owners N         participants (default 16)
//     --live N           resting orders the flow hovers around (default 20000)
//     --max-syncs N      most fdatasync calls any one row makes (default 2000)
//     --runs N           measured runs per row (default 5)
//     --cpu N            pin the thread to CPU N (default: not pinned)
//     --json FILE        also write the results as JSON
//     --label TEXT       free text carried into the report
//
// The same tape of requests is given to the engine in several ways:
//
//   engine alone          no journal: the baseline
//   journal, discarded    every request is encoded and checksummed, and the
//                         bytes thrown away: the cost of the format alone
//   file, batch N         the bytes are written to a file, N requests per
//                         write(), and never synced: survives the program
//                         crashing, not the machine losing power
//   file + fdatasync      the same, with fdatasync after every write: survives
//                         a power cut, at the price of waiting for the device
//
// and then the journal is read back: a full replay into a fresh engine, and a
// snapshot saved and loaded.
//
// How to read it
//
// The fdatasync rows measure the DEVICE the --dir is on, not this code. On a
// consumer SSD one call takes around a millisecond; on a drive that lies about
// flushing, or a filesystem that does not pass the flush on (some virtual and
// network filesystems), it takes microseconds and protects nothing. A number
// here is a number for one disk and one filesystem, and the report names
// neither: say which when quoting it.
//
// Batching does not make a sync faster. It shares one sync between N
// requests, so the cost per request falls as 1/N while each request waits
// longer for its turn. That trade is the whole of group commit.
//
// Rows with fdatasync run only as many requests as --max-syncs allows, since
// a million one-millisecond syncs is a quarter of an hour.
//
// docs/benchmark-method.md applies; read it before quoting a number.
//
// Exit status: 0 ok, 1 usage or I/O error, 3 a journaled run ended in a
// different engine state or its journal did not replay to it, 4 the chosen
// engine is not written yet.

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "bench_env.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/engines.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/journal/file.hpp"
#include "obe/journal/replay.hpp"
#include "obe/journal/snapshot.hpp"
#include "obe/journal/writer.hpp"
#include "obe/util/cpu.hpp"
#include "obe/util/format.hpp"
#include "obe/util/huge_pages.hpp"
#include "obe/util/mapped_file.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;
using Clock = std::chrono::steady_clock;

struct Options {
    gen::FlowConfig flow{.symbols = 100, .owners = 16, .target_live_orders = 20'000};
    std::uint64_t commands = 1'000'000;
    std::uint64_t max_syncs = 2'000;
    std::string engine = "reference";
    std::string dir = ".";
    std::string json_path;
    std::string label;
    int runs = 5;
    int cpu = -1;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: journal_bench [--dir DIR] [--engine NAME] [--seed N] [--commands N] "
                 "[--symbols N]\n"
                 "                     [--owners N] [--live N] [--max-syncs N] [--runs N] "
                 "[--cpu N]\n"
                 "                     [--json FILE] [--label TEXT]\n");
    return status;
}

double ns_since(Clock::time_point start) {
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

enum class Where : std::uint8_t { None, Discard, File, FileSynced };

struct Row {
    std::string name;
    Where where = Where::None;
    std::uint32_t batch = 1;
    std::uint64_t requests = 0;          // timed in each run
    std::vector<double> ns_per_request;  // one per run
    std::uint64_t writes = 0;            // of the last run
    std::uint64_t syncs = 0;
    std::uint64_t bytes = 0;
    double sync_ns = 0;  // time inside fdatasync, last run
    bool state_ok = true;

    [[nodiscard]] bench::Summary summary() const {
        return bench::summarize(ns_per_request, [](double v) { return v; });
    }
};

struct Recovery {
    bool measured = false;
    std::uint64_t records = 0;
    std::size_t journal_bytes = 0;
    double replay_ns = 0;
    bool snapshot = false;
    std::size_t snapshot_bytes = 0;
    std::size_t snapshot_orders = 0;
    double save_ns = 0;  // capture and encode
    double load_ns = 0;  // decode and restore
};

struct Report {
    Options options;
    std::string engine_description;
    bench::Environment env;
    std::size_t huge_bytes = 0;
    std::vector<std::string> warnings;
    std::vector<Row> rows;
    Recovery recovery;
};

struct Discard {
    void operator()(std::span<const std::byte>) const noexcept {}
};

// The file, with the time spent waiting for the device kept apart.
struct TimedFile {
    journal::JournalFile* file;
    bool sync;
    double* sync_ns;

    void operator()(std::span<const std::byte> bytes) const {
        file->write(bytes);
        if (sync) {
            const auto start = Clock::now();
            file->sync();
            *sync_ns += ns_since(start);
        }
    }
};

template <class Impl>
using Engine = typename Impl::template Engine<engine::NullReports, engine::NullMarketData>;

template <class Impl>
struct Fresh {
    engine::NullReports reports;
    engine::NullMarketData market;
    // An engine holds a book for every possible locate; keep it off the stack.
    std::unique_ptr<Engine<Impl>> engine = std::make_unique<Engine<Impl>>(reports, market);
};

// Opens the tape's instruments, as the flow did when the tape was recorded.
template <class Target>
void open_instruments(Target& target, const Options& opt) {
    gen::OrderFlow flow(opt.flow);
    flow.open(target);
}

// The requests, recorded once with the reference engine. Generating them is
// not part of any timing: the generator costs more than the engine does.
std::vector<gen::Command> record_tape(const Options& opt) {
    using Recorder = engine::ReferenceEngine<gen::OrderFlow, engine::NullMarketData>;
    std::vector<gen::Command> tape;
    tape.reserve(opt.commands);
    gen::OrderFlow flow(opt.flow);
    engine::NullMarketData market;
    const auto engine = std::make_unique<Recorder>(flow, market);
    flow.open(*engine);
    for (std::uint64_t i = 0; i < opt.commands; ++i) {
        const gen::Command c = flow.next();
        gen::apply(*engine, c);
        tape.push_back(c);
    }
    return tape;
}

// What the engine holds after the first `requests` of the tape, with no
// journal anywhere near it.
template <class Impl>
std::uint64_t expected_digest(const Options& opt, const std::vector<gen::Command>& tape,
                              std::uint64_t requests) {
    Fresh<Impl> fresh;
    open_instruments(*fresh.engine, opt);
    for (std::uint64_t i = 0; i < requests; ++i) {
        gen::apply(*fresh.engine, tape[i]);
    }
    return journal::state_digest(*fresh.engine);
}

template <class Target>
double time_requests(Target& target, const std::vector<gen::Command>& tape,
                     std::uint64_t requests) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < requests; ++i) {
        gen::apply(target, tape[i]);
    }
    return ns_since(start);
}

// One timed run of one row. Returns the nanoseconds the requests took, with
// the final commit inside the timing: a batch that was never written was
// never recorded.
template <class Impl>
double one_run(const Options& opt, const std::vector<gen::Command>& tape, Row& row,
               const std::string& path, std::uint64_t expected) {
    Fresh<Impl> fresh;
    double ns = 0;
    if (row.where == Where::None) {
        open_instruments(*fresh.engine, opt);
        ns = time_requests(*fresh.engine, tape, row.requests);
    } else if (row.where == Where::Discard) {
        journal::JournalWriter<Discard> writer{Discard{}};
        journal::Journaled journaled(*fresh.engine, writer, row.batch);
        open_instruments(journaled, opt);
        journaled.commit();
        const auto start = Clock::now();
        for (std::uint64_t i = 0; i < row.requests; ++i) {
            gen::apply(journaled, tape[i]);
        }
        journaled.commit();
        ns = ns_since(start);
        row.bytes = writer.bytes();
    } else {
        std::filesystem::remove(path);
        journal::JournalFile file(path);
        row.sync_ns = 0;
        journal::JournalWriter<TimedFile> writer{
            TimedFile{&file, row.where == Where::FileSynced, &row.sync_ns}};
        journal::Journaled journaled(*fresh.engine, writer, row.batch);
        open_instruments(journaled, opt);
        journaled.commit();
        const std::uint64_t writes_before = file.writes();
        const std::uint64_t syncs_before = file.syncs();
        row.sync_ns = 0;
        const auto start = Clock::now();
        for (std::uint64_t i = 0; i < row.requests; ++i) {
            gen::apply(journaled, tape[i]);
        }
        journaled.commit();
        ns = ns_since(start);
        row.writes = file.writes() - writes_before;
        row.syncs = file.syncs() - syncs_before;
        row.bytes = file.size();
    }
    if (journal::state_digest(*fresh.engine) != expected) {
        row.state_ok = false;
    }
    return ns / static_cast<double>(row.requests);
}

// The journal at `path` must replay to the state the run that wrote it ended
// in. Times the replay while it is at it.
template <class Impl>
bool replay_file(const std::string& path, std::uint64_t expected, Recovery* into) {
    Fresh<Impl> fresh;
    const util::MappedFile file(path);
    const auto start = Clock::now();
    const journal::ReplayResult result = journal::replay(file.bytes(), *fresh.engine);
    const double ns = ns_since(start);
    if (into != nullptr) {
        into->measured = true;
        into->records = result.applied;
        into->journal_bytes = file.size();
        into->replay_ns = ns;
        if constexpr (engine::Restorable<Engine<Impl>>) {
            into->snapshot = true;
            auto save = Clock::now();
            const std::vector<std::byte> bytes =
                journal::encode(journal::capture(*fresh.engine, result.next_sequence));
            into->save_ns = ns_since(save);
            into->snapshot_bytes = bytes.size();
            Fresh<Impl> other;
            save = Clock::now();
            const std::optional<journal::EngineState> state = journal::decode(bytes);
            const bool restored = state && journal::restore(*state, *other.engine);
            into->load_ns = ns_since(save);
            into->snapshot_orders = state ? state->orders.size() : 0;
            if (!restored || journal::state_digest(*other.engine) != expected) {
                return false;
            }
        }
    }
    return result.clean() && journal::state_digest(*fresh.engine) == expected;
}

void print_text(const Report& r) {
    const Options& opt = r.options;
    std::printf("journal_bench: what recording every request costs\n");
    std::printf("  engine      %s: %s\n", opt.engine.c_str(), r.engine_description.c_str());
    std::printf("  tape        %s requests, seed %" PRIu64
                ", %u symbols, about %s resting orders\n",
                util::with_commas(opt.commands).c_str(), opt.flow.seed, opt.flow.symbols,
                util::with_commas(opt.flow.target_live_orders).c_str());
    std::printf("  journal in  %s\n", std::filesystem::absolute(opt.dir).string().c_str());
    std::printf("  runs        %d per row; the median is shown\n", opt.runs);
    if (!opt.label.empty()) {
        std::printf("  label       %s\n", opt.label.c_str());
    }
    bench::print_environment(r.env, r.huge_bytes);

    std::printf("\n  %-28s %11s %11s %7s %12s %9s %9s %10s\n", "", "requests", "ns/request",
                "spread", "requests/s", "writes", "syncs", "us/sync");
    for (const Row& row : r.rows) {
        const bench::Summary s = row.summary();
        std::printf("  %-28s %11s %11.0f %6.1f%% %12s", row.name.c_str(),
                    util::with_commas(row.requests).c_str(), s.median, s.spread_percent(),
                    util::with_commas(static_cast<std::uint64_t>(1e9 / s.median)).c_str());
        if (row.where == Where::File || row.where == Where::FileSynced) {
            std::printf(" %9s %9s", util::with_commas(row.writes).c_str(),
                        util::with_commas(row.syncs).c_str());
            if (row.syncs > 0) {
                std::printf(" %10.1f", row.sync_ns / 1e3 / static_cast<double>(row.syncs));
            } else {
                std::printf(" %10s", "-");
            }
        } else {
            std::printf(" %9s %9s %10s", "-", "-", "-");
        }
        std::printf("%s\n", row.state_ok ? "" : "   WRONG STATE");
    }
    std::printf(
        "\n  A crash of the program loses at most (batch - 1) requests, those not yet\n"
        "  written. A power cut loses whatever was written and not synced. Either way\n"
        "  nobody may be told the result of a request that could still be lost.\n");

    const Recovery& rec = r.recovery;
    if (rec.measured) {
        std::printf("\nrecovery\n");
        std::printf("  replay      %s records (%s bytes) in %.1f ms: %s records/s\n",
                    util::with_commas(rec.records).c_str(),
                    util::with_commas(rec.journal_bytes).c_str(), rec.replay_ns / 1e6,
                    util::with_commas(static_cast<std::uint64_t>(static_cast<double>(rec.records) *
                                                                 1e9 / rec.replay_ns))
                        .c_str());
        if (rec.snapshot) {
            std::printf(
                "  snapshot    %s resting orders in %s bytes: saved in %.2f ms, loaded in "
                "%.2f ms\n",
                util::with_commas(rec.snapshot_orders).c_str(),
                util::with_commas(rec.snapshot_bytes).c_str(), rec.save_ns / 1e6,
                rec.load_ns / 1e6);
            std::printf("              (in memory: writing it to disk adds one fdatasync)\n");
        } else {
            std::printf("  snapshot    this engine cannot be saved to one\n");
        }
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
    std::fprintf(f, "{\n  \"benchmark\": \"journal_bench\",\n");
    std::fprintf(f, "  \"label\": \"%s\",\n", bench::json_escape(opt.label).c_str());
    std::fprintf(f, "  \"engine\": \"%s\",\n", bench::json_escape(opt.engine).c_str());
    bench::write_environment_json(f, r.env, r.huge_bytes);
    std::fprintf(f, "  \"seed\": %" PRIu64 ",\n", opt.flow.seed);
    std::fprintf(f, "  \"commands\": %" PRIu64 ",\n", opt.commands);
    std::fprintf(f, "  \"symbols\": %u,\n", opt.flow.symbols);
    std::fprintf(f, "  \"target_live_orders\": %u,\n", opt.flow.target_live_orders);
    std::fprintf(f, "  \"runs\": %d,\n", opt.runs);
    std::fprintf(f, "  \"rows\": [\n");
    for (std::size_t i = 0; i < r.rows.size(); ++i) {
        const Row& row = r.rows[i];
        const bench::Summary s = row.summary();
        std::fprintf(f,
                     "    {\"name\": \"%s\", \"batch\": %u, \"requests\": %" PRIu64
                     ", \"ns_per_request_median\": %.1f, \"ns_per_request_min\": %.1f, "
                     "\"ns_per_request_max\": %.1f, \"writes\": %" PRIu64 ", \"syncs\": %" PRIu64
                     ", \"sync_ns\": %.0f, \"bytes\": %" PRIu64 ", \"state_ok\": %s}%s\n",
                     bench::json_escape(row.name).c_str(), row.batch, row.requests, s.median, s.min,
                     s.max, row.writes, row.syncs, row.sync_ns, row.bytes,
                     row.state_ok ? "true" : "false", i + 1 < r.rows.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n");
    const Recovery& rec = r.recovery;
    std::fprintf(f,
                 "  \"recovery\": {\"records\": %" PRIu64
                 ", \"journal_bytes\": %zu, "
                 "\"replay_ns\": %.0f, \"snapshot\": %s, \"snapshot_bytes\": %zu, "
                 "\"snapshot_orders\": %zu, \"save_ns\": %.0f, \"load_ns\": %.0f},\n",
                 rec.records, rec.journal_bytes, rec.replay_ns, rec.snapshot ? "true" : "false",
                 rec.snapshot_bytes, rec.snapshot_orders, rec.save_ns, rec.load_ns);
    std::fprintf(f, "  \"warnings\": [");
    for (std::size_t i = 0; i < r.warnings.size(); ++i) {
        std::fprintf(f, "%s\"%s\"", i == 0 ? "" : ", ", bench::json_escape(r.warnings[i]).c_str());
    }
    std::fprintf(f, "]\n}\n");
    return std::fclose(f) == 0;
}

template <class Impl>
int measure(const Options& opt, Report& report) {
    report.engine_description = std::string(Impl::kDescription);
    const std::vector<gen::Command> tape = record_tape(opt);
    const std::string path = (std::filesystem::path(opt.dir) /
                              ("journal_bench_" + std::to_string(::getpid()) + ".journal"))
                                 .string();

    const auto add = [&](std::string name, Where where, std::uint32_t batch) {
        Row row;
        row.name = std::move(name);
        row.where = where;
        row.batch = batch;
        row.requests = opt.commands;
        if (where == Where::FileSynced) {
            row.requests = std::min<std::uint64_t>(opt.commands, opt.max_syncs * batch);
        }
        report.rows.push_back(std::move(row));
    };
    add("engine alone", Where::None, 1);
    add("journal, discarded", Where::Discard, 1);
    for (const std::uint32_t batch : {1U, 16U, 256U, 4096U}) {
        add("file, batch " + std::to_string(batch), Where::File, batch);
    }
    for (const std::uint32_t batch : {1U, 16U, 256U, 4096U}) {
        add("file + fdatasync, batch " + std::to_string(batch), Where::FileSynced, batch);
    }

    bool ok = true;
    const std::uint64_t whole = expected_digest<Impl>(opt, tape, opt.commands);
    for (Row& row : report.rows) {
        const std::uint64_t expected =
            row.requests == opt.commands ? whole : expected_digest<Impl>(opt, tape, row.requests);
        static_cast<void>(one_run<Impl>(opt, tape, row, path, expected));  // warm-up
        for (int i = 0; i < opt.runs; ++i) {
            row.ns_per_request.push_back(one_run<Impl>(opt, tape, row, path, expected));
        }
        if (row.where == Where::File || row.where == Where::FileSynced) {
            // The last unsynced row holds the whole tape: time recovery on it.
            const bool last_whole = row.where == Where::File && row.batch == 4096;
            if (!replay_file<Impl>(path, expected, last_whole ? &report.recovery : nullptr)) {
                row.state_ok = false;
            }
        }
        ok = ok && row.state_ok;
        if (row.summary().spread_percent() > 10.0) {
            report.warnings.emplace_back("'" + row.name + "' varied by more than 10% across runs");
        }
    }
    std::filesystem::remove(path);
    report.huge_bytes = util::process_huge_bytes();

    print_text(report);
    if (!opt.json_path.empty() && !write_json(report)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", opt.json_path.c_str());
        return 1;
    }
    if (!ok) {
        std::printf("\nRESULT: a journaled run did not end in the state of the plain one\n");
        return 3;
    }
    return 0;
}

template <class T>
bool parse_number(std::string_view text, T& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
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
        if (arg == "--engine" && has_next) {
            opt.engine = argv[++i];
        } else if (arg == "--dir" && has_next) {
            opt.dir = argv[++i];
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
        } else if (arg == "--max-syncs" && has_value) {
            opt.max_syncs = value;
            ++i;
        } else if (arg == "--runs" && has_value) {
            opt.runs = static_cast<int>(value);
            ++i;
        } else if (arg == "--cpu" && has_value) {
            opt.cpu = static_cast<int>(value);
            ++i;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (opt.runs < 1 || opt.commands == 0 || opt.max_syncs == 0) {
        std::fprintf(stderr, "error: --runs, --commands and --max-syncs must be at least 1\n");
        return 1;
    }
    if (!std::filesystem::is_directory(opt.dir)) {
        std::fprintf(stderr, "error: '%s' is not a directory\n", opt.dir.c_str());
        return 1;
    }

    Report report;
    report.options = opt;
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

    int status = 1;
    const bool known = engine::with_engine(opt.engine, [&]<class Impl>(std::type_identity<Impl>) {
        status = measure<Impl>(opt, report);
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
