// pipeline_bench: does the three-thread pipeline beat one thread?
//
//   pipeline_bench <file> [options]
//     --queue NAME       mutex | mutex-spin | ring (default: mutex);
//                        queue_bench --list prints them
//     --impl NAME        which book implementation (default: reference)
//     --reserve N        pre-size the order store for N resting orders
//     --capacity N       capacity of both queues (default 65536)
//     --cpus P,B,C       pin the parser, book and consumer threads to these
//                        CPUs; the single-threaded runs use the first
//     --readers N        threads that keep reading the top-of-book board
//                        while the pipeline runs (default 0)
//     --runs N           measured runs of each (default 5)
//     --warmup N         unmeasured runs of each before them (default 1)
//     --no-copy          time against the page cache instead of a private copy
//     --json FILE        also write the results as JSON
//     --label TEXT       free text carried into the report
//
// It answers the question phase 6 ends on. The same file is replayed two
// ways, in one process, on the same machine, minutes apart at most:
//
//   single   one thread: parse, update the book, and hash every
//            best-bid-and-offer update
//   pipeline three threads joined by two queues: parse | book | hash
//
// Both do the same work and must produce the same hash; if they do not, the
// pipeline's timings are void. Then the two throughputs are set side by side.
//
// The pipeline may well lose. Each message costs tens of nanoseconds to
// process, and handing it to another core costs a cache line transfer in each
// direction. Either result is a finding; write it in the log with the numbers
// and the CPUs the threads ran on.
//
// What is timed for the pipeline is from just before its threads are started
// to just after the last is joined, so thread start-up is inside. On a file of
// any real size that is noise.
//
// Exit status: 0 ok, 1 usage or I/O error, 2 the file is truncated or corrupt,
// 3 the pipeline's output differed from the single thread's, 4 the chosen
// queue or book is not written yet.

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "bench_env.hpp"
#include "bench_input.hpp"
#include "obe/book/bbo_hash.hpp"
#include "obe/book/book_manager.hpp"
#include "obe/book/implementations.hpp"
#include "obe/feed/parser.hpp"
#include "obe/pipeline/pipeline.hpp"
#include "obe/pipeline/top_of_book.hpp"
#include "obe/util/channels.hpp"
#include "obe/util/cpu.hpp"
#include "obe/util/format.hpp"
#include "obe/util/huge_pages.hpp"
#include "obe/util/mapped_file.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;
using bench::summarize;
using bench::Summary;

struct Options {
    std::string path;
    std::string queue = "mutex";
    std::string impl = "reference";
    std::string json_path;
    std::string label;
    std::size_t reserve = 0;
    std::size_t capacity = std::size_t{1} << 16;
    std::array<int, 3> cpus{-1, -1, -1};
    int readers = 0;
    int runs = 5;
    int warmup = 1;
    bool copy = true;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: pipeline_bench <uncompressed ITCH 5.0 file> [--queue NAME] [--impl NAME]\n"
                 "                      [--reserve N] [--capacity N] [--cpus P,B,C] "
                 "[--readers N]\n"
                 "                      [--runs N] [--warmup N] [--no-copy] [--json FILE] "
                 "[--label TEXT]\n");
    return status;
}

template <class T>
bool parse_number(std::string_view text, T& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

bool parse_cpus(std::string_view text, std::array<int, 3>& out) {
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t comma = text.find(',');
        const bool last = i == 2;
        if ((comma == std::string_view::npos) != last) {
            return false;
        }
        if (!parse_number(text.substr(0, comma), out[i]) || out[i] < 0) {
            return false;
        }
        if (!last) {
            text.remove_prefix(comma + 1);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The two ways to replay
// ---------------------------------------------------------------------------

struct Run {
    double seconds = 0;
    double msgs_per_s = 0;
    std::uint64_t hash = 0;
    util::ChannelStats events;    // pipeline only
    util::ChannelStats updates;   // pipeline only
    std::uint64_t reads = 0;      // board reads by the reader threads
    std::uint64_t board_sum = 0;  // of what they read; see pipeline_run
};

struct Baseline {
    feed::ParseResult parse;
    std::uint64_t hash = 0;
    std::uint64_t updates = 0;
    book::Counters counters;
    bool audit_clean = false;
};

// Everything on the calling thread: the work the pipeline splits in three.
template <class Impl>
Run single_run(const Options& opt, std::span<const std::byte> buf, Baseline* baseline) {
    using Manager = book::BookManager<typename Impl::Store, typename Impl::Levels, book::BboHasher>;
    const auto manager =
        std::make_unique<Manager>(book::make_store<typename Impl::Store>(opt.reserve));
    feed::ItchParser parser(*manager);

    const auto start = std::chrono::steady_clock::now();
    // ---- timed region starts ----
    const feed::ParseResult parsed = parser.parse(buf);
    // ---- timed region ends ----
    const auto end = std::chrono::steady_clock::now();

    Run run;
    run.seconds = std::chrono::duration<double>(end - start).count();
    run.msgs_per_s = run.seconds > 0 ? static_cast<double>(parsed.messages) / run.seconds : 0;
    run.hash = manager->listener().combined();
    if (baseline != nullptr) {
        baseline->parse = parsed;
        baseline->hash = run.hash;
        baseline->updates = manager->listener().total_events();
        baseline->counters = manager->counters();
        baseline->audit_clean = manager->audit().clean();
    }
    return run;
}

template <class Kind, class Impl>
Run pipeline_run(const Options& opt, std::span<const std::byte> buf) {
    pipeline::TopOfBookBoard board;
    pipeline::PipelineConfig cfg;
    cfg.event_capacity = opt.capacity;
    cfg.update_capacity = opt.capacity;
    cfg.cpus = opt.cpus;
    cfg.reserve = opt.reserve;
    cfg.board = &board;

    // Readers sweep the whole board for as long as the pipeline runs. They are
    // not pinned: where they land is part of what "somebody is watching"
    // means on this machine.
    std::atomic<bool> done{false};
    std::vector<std::uint64_t> reads(static_cast<std::size_t>(opt.readers), 0);
    // What the readers read is summed and kept, so the reads cannot be
    // optimized away as unused.
    std::vector<std::uint64_t> sums(reads.size(), 0);
    std::vector<std::thread> readers;
    readers.reserve(reads.size());
    for (std::size_t r = 0; r < reads.size(); ++r) {
        readers.emplace_back([&board, &done, &count = reads[r], &sum = sums[r]] {
            std::uint64_t local = 0;
            std::uint64_t seen = 0;
            while (!done.load(std::memory_order_acquire)) {
                for (std::size_t i = 0; i < pipeline::TopOfBookBoard::kLocates; ++i) {
                    seen += board.read(static_cast<Locate>(i)).bbo.bid_price;
                    ++local;
                }
            }
            count = local;
            sum = seen;
        });
    }
    struct StopReaders {
        std::atomic<bool>& flag;
        std::vector<std::thread>& threads;
        ~StopReaders() {
            flag.store(true, std::memory_order_release);
            for (std::thread& t : threads) {
                t.join();
            }
        }
    };

    pipeline::PipelineResult result;
    {
        const StopReaders stop{done, readers};
        result = pipeline::run_pipeline<Kind::template Channel, Impl>(buf, cfg);
    }

    Run run;
    run.seconds = result.seconds;
    run.msgs_per_s = run.seconds > 0 ? static_cast<double>(result.parse.messages) / run.seconds : 0;
    run.hash = result.hasher->combined();
    run.events = result.event_channel;
    run.updates = result.update_channel;
    for (const std::uint64_t n : reads) {
        run.reads += n;
    }
    for (const std::uint64_t n : sums) {
        run.board_sum += n;
    }
    return run;
}

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------

struct Report {
    Options options;
    std::string queue_description;
    std::string impl_description;
    bench::Environment env;
    std::size_t file_bytes = 0;
    std::size_t huge_bytes = 0;
    Baseline baseline;
    std::vector<Run> single;
    std::vector<Run> piped;
    std::vector<std::string> warnings;

    [[nodiscard]] bool same_output() const {
        for (const Run& r : piped) {
            if (r.hash != baseline.hash) {
                return false;
            }
        }
        return true;
    }
};

void print_runs(const char* title, const std::vector<Run>& runs, bool with_waits) {
    const Summary rate = summarize(runs, [](const Run& x) { return x.msgs_per_s; });
    std::printf("\n  %s\n", title);
    std::printf("  %-6s %12s %9s", "run", "msgs/s", "seconds");
    if (with_waits) {
        std::printf(" %13s %13s %13s %13s", "parser waits", "book starved", "book waits",
                    "consumer idle");
    }
    std::printf("\n");
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const Run& x = runs[i];
        std::printf("  %-6zu %12.0f %9.3f", i + 1, x.msgs_per_s, x.seconds);
        if (with_waits) {
            std::printf(" %13s %13s %13s %13s", util::with_commas(x.events.push_waits).c_str(),
                        util::with_commas(x.events.pop_waits).c_str(),
                        util::with_commas(x.updates.push_waits).c_str(),
                        util::with_commas(x.updates.pop_waits).c_str());
        }
        std::printf("\n");
    }
    std::printf("  %-6s %12.0f\n", "median", rate.median);
    std::printf("  %-6s %12.0f\n", "min", rate.min);
    std::printf("  %-6s %12.0f\n", "max", rate.max);
    std::printf("  spread: %.1f%% of the median\n", rate.spread_percent());
}

void print_text(const Report& r) {
    const Summary single = summarize(r.single, [](const Run& x) { return x.msgs_per_s; });
    const Summary piped = summarize(r.piped, [](const Run& x) { return x.msgs_per_s; });

    std::printf("pipeline_bench%s%s\n",
                r.options.label.empty() ? "" : "  label: ", r.options.label.c_str());
    std::printf("  file        %s\n", r.options.path.c_str());
    std::printf("              %s bytes, %s messages, %s\n",
                util::with_commas(r.file_bytes).c_str(),
                util::with_commas(r.baseline.parse.messages).c_str(),
                r.options.copy ? "private in-memory copy" : "page cache (--no-copy)");
    std::printf("  book        %s: %s\n", r.options.impl.c_str(), r.impl_description.c_str());
    std::printf("  queues      %s: %s\n", r.options.queue.c_str(), r.queue_description.c_str());
    std::printf("              capacity %s each; %s best bid/offer updates cross the second\n",
                util::with_commas(r.options.capacity).c_str(),
                util::with_commas(r.baseline.updates).c_str());
    std::printf("  runs        %d measured after %d warm-up, of each\n", r.options.runs,
                r.options.warmup);
    bench::print_environment(r.env, r.huge_bytes);
    if (r.options.cpus[0] >= 0) {
        std::printf(
            "  threads     parser on CPU %d, book on CPU %d, consumer on CPU %d; the\n"
            "              single-threaded runs on CPU %d\n",
            r.options.cpus[0], r.options.cpus[1], r.options.cpus[2], r.options.cpus[0]);
    }
    if (r.options.readers != 0) {
        std::printf("  readers     %d thread(s) reading the top-of-book board throughout\n",
                    r.options.readers);
    }
    std::printf("  book check  %s, best bid/offer stream hash %016" PRIx64 "\n",
                r.baseline.counters.clean() && r.baseline.audit_clean ? "all invariants zero"
                                                                      : "INVARIANTS VIOLATED",
                r.baseline.hash);
    std::printf("              pipeline output %s\n",
                r.same_output() ? "identical to the single thread's"
                                : "DIFFERS: THE PIPELINE TIMINGS BELOW ARE VOID");

    print_runs("single thread: parse, book and hash on one core", r.single, false);
    print_runs("pipeline: parse | book | hash on three threads", r.piped, true);

    if (single.median > 0) {
        const double ratio = piped.median / single.median;
        std::printf("\n  pipeline / single thread = %.2f  (%s)\n", ratio,
                    ratio >= 1.0 ? "the pipeline is faster" : "the single thread is faster");
    }
    std::printf("  \"parser waits\": pushes that found the first queue full (the book thread is\n");
    std::printf(
        "  the slower side). \"book starved\": pops that found it empty (the parser is).\n");
    std::printf("  \"book waits\" and \"consumer idle\" are the same for the second queue.\n");
    if (r.options.readers != 0 && !r.piped.empty()) {
        std::printf("  board reads by the reader threads in the last run: %s\n",
                    util::with_commas(r.piped.back().reads).c_str());
    }

    for (const std::string& w : r.warnings) {
        std::printf("\n  WARNING: %s\n", w.c_str());
    }
}

void json_runs(std::FILE* f, const char* name, const std::vector<Run>& runs) {
    std::fprintf(f, "  \"%s\": [\n", name);
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const Run& x = runs[i];
        std::fprintf(f,
                     "    {\"msgs_per_s\": %.0f, \"seconds\": %.6f, \"event_push_waits\": %zu, "
                     "\"event_pop_waits\": %zu, \"update_push_waits\": %zu, "
                     "\"update_pop_waits\": %zu}%s\n",
                     x.msgs_per_s, x.seconds, x.events.push_waits, x.events.pop_waits,
                     x.updates.push_waits, x.updates.pop_waits, i + 1 == runs.size() ? "" : ",");
    }
    std::fprintf(f, "  ],\n");
}

bool write_json(const Report& r) {
    std::FILE* f = std::fopen(r.options.json_path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    const Summary single = summarize(r.single, [](const Run& x) { return x.msgs_per_s; });
    const Summary piped = summarize(r.piped, [](const Run& x) { return x.msgs_per_s; });
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"benchmark\": \"pipeline_bench\",\n");
    std::fprintf(f, "  \"label\": \"%s\",\n", bench::json_escape(r.options.label).c_str());
    std::fprintf(f, "  \"file\": \"%s\",\n", bench::json_escape(r.options.path).c_str());
    std::fprintf(f, "  \"file_bytes\": %zu,\n", r.file_bytes);
    std::fprintf(f, "  \"messages\": %" PRIu64 ",\n", r.baseline.parse.messages);
    std::fprintf(f, "  \"workload\": \"pipeline/%s/%s\",\n",
                 bench::json_escape(r.options.queue).c_str(),
                 bench::json_escape(r.options.impl).c_str());
    std::fprintf(f, "  \"capacity\": %zu,\n", r.options.capacity);
    std::fprintf(f, "  \"reserve\": %zu,\n", r.options.reserve);
    std::fprintf(f, "  \"readers\": %d,\n", r.options.readers);
    std::fprintf(f, "  \"cpus\": [%d, %d, %d],\n", r.options.cpus[0], r.options.cpus[1],
                 r.options.cpus[2]);
    std::fprintf(f, "  \"in_memory_copy\": %s,\n", r.options.copy ? "true" : "false");
    std::fprintf(f, "  \"warmup_runs\": %d,\n", r.options.warmup);
    bench::write_environment_json(f, r.env, r.huge_bytes);
    std::fprintf(f, "  \"bbo_hash\": \"%016" PRIx64 "\",\n", r.baseline.hash);
    std::fprintf(f, "  \"matches_single_thread\": %s,\n", r.same_output() ? "true" : "false");
    std::fprintf(f, "  \"single_median_msgs_per_s\": %.0f,\n", single.median);
    std::fprintf(f, "  \"median_msgs_per_s\": %.0f,\n", piped.median);
    std::fprintf(f, "  \"min_msgs_per_s\": %.0f,\n", piped.min);
    std::fprintf(f, "  \"max_msgs_per_s\": %.0f,\n", piped.max);
    std::fprintf(f, "  \"pipeline_over_single\": %.4f,\n",
                 single.median > 0 ? piped.median / single.median : 0.0);
    json_runs(f, "single_runs", r.single);
    json_runs(f, "runs", r.piped);
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

template <class Kind, class Impl>
int measure(const Options& opt, Report& report, std::span<const std::byte> buf) {
    report.queue_description = std::string(Kind::kDescription);
    report.impl_description = std::string(Impl::kDescription);

    // The single-threaded runs, on a thread of their own so that it can be
    // pinned to the parser's CPU without moving the main thread.
    std::exception_ptr single_error;
    std::thread single([&] {
        try {
            if (opt.cpus[0] >= 0 && !util::pin_to_cpu(opt.cpus[0])) {
                report.warnings.emplace_back("could not pin to CPU " + std::to_string(opt.cpus[0]));
            }
            // The first warm-up run is also where the reference answers come from.
            static_cast<void>(single_run<Impl>(opt, buf, &report.baseline));
            for (int i = 1; i < opt.warmup; ++i) {
                static_cast<void>(single_run<Impl>(opt, buf, nullptr));
            }
            for (int i = 0; i < opt.runs; ++i) {
                report.single.push_back(single_run<Impl>(opt, buf, nullptr));
            }
        } catch (...) {
            single_error = std::current_exception();
        }
    });
    single.join();
    if (single_error) {
        std::rethrow_exception(single_error);
    }

    if (!report.baseline.parse.ok()) {
        const std::string_view why = feed::to_string(report.baseline.parse.status);
        std::fprintf(stderr, "error: %.*s at byte offset %zu, after %" PRIu64 " good messages\n",
                     static_cast<int>(why.size()), why.data(), report.baseline.parse.offset,
                     report.baseline.parse.messages);
        return 2;
    }

    for (int i = 0; i < opt.warmup; ++i) {
        static_cast<void>(pipeline_run<Kind, Impl>(opt, buf));
    }
    for (int i = 0; i < opt.runs; ++i) {
        report.piped.push_back(pipeline_run<Kind, Impl>(opt, buf));
    }
    report.huge_bytes = util::process_huge_bytes();

    const Summary single_rate = summarize(report.single, [](const Run& x) { return x.msgs_per_s; });
    const Summary piped_rate = summarize(report.piped, [](const Run& x) { return x.msgs_per_s; });
    if (single_rate.spread_percent() > 5.0 || piped_rate.spread_percent() > 5.0) {
        report.warnings.emplace_back(
            "throughput varied by more than 5% across runs: the ratio above is not firm");
    }

    print_text(report);
    if (!opt.json_path.empty() && !write_json(report)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", opt.json_path.c_str());
        return 1;
    }
    if (!report.baseline.counters.clean() || !report.baseline.audit_clean) {
        return 3;
    }
    return report.same_output() ? 0 : 3;
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
        if (arg == "--no-copy") {
            opt.copy = false;
        } else if (arg == "--queue" && has_next) {
            opt.queue = argv[++i];
        } else if (arg == "--impl" && has_next) {
            opt.impl = argv[++i];
        } else if (arg == "--json" && has_next) {
            opt.json_path = argv[++i];
        } else if (arg == "--label" && has_next) {
            opt.label = argv[++i];
        } else if (arg == "--cpus" && has_next) {
            if (!parse_cpus(argv[++i], opt.cpus)) {
                std::fprintf(stderr, "error: --cpus needs three CPU numbers, as in --cpus 2,4,6\n");
                return 1;
            }
        } else if (arg == "--reserve" && has_value) {
            opt.reserve = static_cast<std::size_t>(value);
            ++i;
        } else if (arg == "--capacity" && has_value) {
            opt.capacity = static_cast<std::size_t>(value);
            ++i;
        } else if (arg == "--readers" && has_value) {
            opt.readers = static_cast<int>(value);
            ++i;
        } else if (arg == "--runs" && has_value) {
            opt.runs = static_cast<int>(value);
            ++i;
        } else if (arg == "--warmup" && has_value) {
            opt.warmup = static_cast<int>(value);
            ++i;
        } else if (!arg.starts_with("-") && opt.path.empty()) {
            opt.path = arg;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (opt.path.empty()) {
        return usage(stderr, 1);
    }
    if (opt.runs < 1 || opt.warmup < 1 || opt.capacity == 0 || opt.readers < 0 ||
        opt.readers > 64) {
        std::fprintf(stderr,
                     "error: --runs, --warmup and --capacity must be at least 1, and "
                     "--readers between 0 and 64\n");
        return 1;
    }

    // The file, fully in memory before any clock starts.
    std::unique_ptr<bench::MemoryCopy> copy;
    std::unique_ptr<util::MappedFile> mapped;
    std::span<const std::byte> buf;
    if (opt.copy) {
        copy = std::make_unique<bench::MemoryCopy>(opt.path);
        buf = copy->bytes();
    } else {
        mapped = std::make_unique<util::MappedFile>(opt.path);
        buf = mapped->bytes();
        static_cast<void>(bench::touch_pages(buf));
    }
    if (util::looks_gzipped(buf)) {
        std::fprintf(stderr, "error: '%s' is gzip-compressed. Decompress it first.\n",
                     opt.path.c_str());
        return 1;
    }

    Report report;
    report.options = opt;
    report.file_bytes = buf.size();
    report.env = bench::gather_environment(opt.cpus[0], opt.cpus[0] >= 0);
    if (opt.cpus[0] < 0) {
        report.warnings.emplace_back(
            "the threads are not pinned (--cpus P,B,C): where the scheduler puts them "
            "decides the result");
    }
    if (report.env.logical_cpus < 3) {
        report.warnings.emplace_back(
            "this machine has fewer than three CPUs: the pipeline's threads share cores "
            "and cannot win");
    }
    if (!bench::is_release_build() || bench::is_instrumented_build()) {
        report.warnings.emplace_back(
            "this is not an optimized, uninstrumented build: the timings mean nothing");
    }
    if (opt.runs < 5) {
        report.warnings.emplace_back("fewer than 5 measured runs: not enough to quote");
    }

    int status = 1;
    bool known_impl = false;
    const bool known_queue =
        util::with_channel(opt.queue, [&]<class Kind>(std::type_identity<Kind>) {
            known_impl =
                book::with_implementation(opt.impl, [&]<class Impl>(std::type_identity<Impl>) {
                    status = measure<Kind, Impl>(opt, report, buf);
                });
        });
    if (!known_queue) {
        std::fprintf(stderr, "error: no queue called '%s'. Try queue_bench --list.\n",
                     opt.queue.c_str());
        return 1;
    }
    if (!known_impl) {
        std::fprintf(stderr, "error: no implementation called '%s'. Try book_replay --list.\n",
                     opt.impl.c_str());
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
