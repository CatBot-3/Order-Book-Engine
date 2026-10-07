// tick_bench: what a tick codec saves and what it costs.
//
//   tick_bench [options]
//     --codec NAME       measure this codec only (default: every codec that
//                        is written)
//     --file FILE        take the ticks from an ITCH file, replayed through
//                        the book (default: a generated market)
//     --max-ticks N      with --file, keep the first N ticks (default 20000000)
//     --ticks N          generated ticks (default 2000000)
//     --securities N     generated securities (default 500)
//     --seed N           seed of the generator (default 1)
//     --queries N        range queries per row (default 2000)
//     --runs N           measured runs per row (default 5)
//     --cpu N            pin the thread to CPU N (default: not pinned)
//     --json FILE        also write the results as JSON
//     --label TEXT       free text carried into the report
//
// For each codec, at three block sizes, the same ticks are written to a store
// in memory, read back whole, and asked for short ranges of time:
//
//   bytes/tick     the encoded ticks alone, and the whole file with its block
//                  headers and index
//   write ns/tick  encoding, checksumming and laying out the blocks
//   read ns/tick   a scan of everything: checksum, decode, hand over
//   query          the mean time of a range a thousandth of the store long,
//                  and how many blocks it had to decode
//
// How to read it
//
// A codec trades bytes for time, and the block size trades compression for
// seek: a query decodes whole blocks, so bigger blocks make each query read
// more ticks it does not want. The three block sizes are there so that the
// trade is a row in a table and not an opinion.
//
// Everything is in memory. Nothing here measures a disk: a real query pays
// for reading its blocks from storage too, and smaller files are cheaper to
// read by the factor they are smaller, which is the reason to compress in the
// first place and is not in these numbers.
//
// The default ticks are generated (obe/gen/tick_gen.hpp) and are not a
// market. Quote a compression ratio only from --file on a real day.
//
// docs/benchmark-method.md applies; read it before quoting a number.
//
// Exit status: 0 ok, 1 usage or I/O error, 2 the ITCH file is corrupt, 3 a
// store did not give back the ticks it was given, 4 the chosen codec is not
// written yet.

#include <algorithm>
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
#include <type_traits>
#include <vector>

#include "bench_env.hpp"
#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/feed/parser.hpp"
#include "obe/gen/rng.hpp"
#include "obe/gen/tick_gen.hpp"
#include "obe/store/codecs.hpp"
#include "obe/store/tick_codec.hpp"
#include "obe/store/tick_file.hpp"
#include "obe/util/cpu.hpp"
#include "obe/util/format.hpp"
#include "obe/util/huge_pages.hpp"
#include "obe/util/mapped_file.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;
using store::Tick;
using Clock = std::chrono::steady_clock;

struct Options {
    std::string codec;  // empty: every codec that is written
    std::string file;
    std::string json_path;
    std::string label;
    std::uint64_t max_ticks = 20'000'000;
    std::uint64_t ticks = 2'000'000;
    std::uint32_t securities = 500;
    std::uint64_t seed = 1;
    std::uint64_t queries = 2'000;
    int runs = 5;
    int cpu = -1;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: tick_bench [--codec NAME] [--file <ITCH file>] [--max-ticks N] "
                 "[--ticks N]\n"
                 "                  [--securities N] [--seed N] [--queries N] [--runs N] "
                 "[--cpu N]\n"
                 "                  [--json FILE] [--label TEXT]\n");
    return status;
}

double ns_since(Clock::time_point start) {
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

// Everything about a tick, folded into one number: what a scan is checked
// against, and what keeps the compiler from discarding the decoding.
constexpr std::uint64_t fold(std::uint64_t acc, const Tick& t) noexcept {
    acc = (acc ^ t.timestamp) * 0x100000001b3ULL;
    acc = (acc ^ t.locate) * 0x100000001b3ULL;
    acc = (acc ^ t.bbo.bid_price) * 0x100000001b3ULL;
    acc = (acc ^ t.bbo.bid_qty) * 0x100000001b3ULL;
    acc = (acc ^ t.bbo.ask_price) * 0x100000001b3ULL;
    acc = (acc ^ t.bbo.ask_qty) * 0x100000001b3ULL;
    return acc;
}

struct AppendTo {
    std::vector<std::byte>* out;
    void operator()(std::span<const std::byte> bytes) const {
        out->insert(out->end(), bytes.begin(), bytes.end());
    }
};

struct Row {
    std::string codec;
    std::uint32_t block = 0;
    std::uint64_t file_bytes = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t blocks = 0;
    std::vector<double> write_ns;  // per tick, one per run
    std::vector<double> read_ns;   // per tick, one per run
    std::vector<double> query_ns;  // per query, one per run
    double blocks_per_query = 0;
    double ticks_per_query = 0;
    bool ok = true;
};

struct Report {
    Options options;
    bench::Environment env;
    std::size_t huge_bytes = 0;
    std::string source;
    std::uint64_t ticks = 0;
    std::uint64_t securities = 0;
    Nanos earliest = 0;
    Nanos latest = 0;
    std::vector<Row> rows;
    std::vector<std::string> not_written;
    std::vector<std::string> warnings;
};

// A book listener that keeps the first `limit` updates.
struct KeepTicks {
    std::vector<Tick>* ticks = nullptr;
    std::uint64_t limit = 0;
    void on_bbo(const book::BboUpdate& update) const {
        if (ticks->size() < limit) {
            ticks->push_back(update);
        }
    }
};

template <class Codec>
Row measure(const Options& opt, const std::vector<Tick>& ticks, std::uint32_t block,
            std::uint64_t expected, Nanos earliest, Nanos latest) {
    Row row;
    row.codec = std::string(Codec::kName);
    row.block = block;
    const double count = static_cast<double>(ticks.size());

    std::vector<std::byte> file;
    // Room for the worst case, so that no run is timed growing the vector.
    file.reserve(ticks.size() * (Codec::kMaxTickSize + 1) + (std::size_t{1} << 20));
    for (int run = -1; run < opt.runs; ++run) {  // run -1 is the warm-up
        file.clear();
        const Clock::time_point start = Clock::now();
        store::TickWriter<Codec, AppendTo> writer(AppendTo{&file}, block);
        for (const Tick& tick : ticks) {
            writer.append(tick);
        }
        writer.finish();
        const double ns = ns_since(start);
        if (run >= 0) {
            row.write_ns.push_back(ns / count);
        }
        row.file_bytes = writer.bytes();
        row.payload_bytes = writer.payload_bytes();
        row.blocks = writer.blocks();
    }

    const store::TickReader<Codec> reader(file);
    row.ok = reader.status() == store::OpenStatus::Complete && reader.ticks() == ticks.size();
    for (int run = -1; run < opt.runs && row.ok; ++run) {
        std::uint64_t acc = 0;
        const Clock::time_point start = Clock::now();
        const store::ScanResult result = reader.scan([&acc](const Tick& tick) {
            acc = fold(acc, tick);
            return true;
        });
        const double ns = ns_since(start);
        if (run >= 0) {
            row.read_ns.push_back(ns / count);
        }
        row.ok = result.ok && result.ticks == ticks.size() && acc == expected;
    }

    // Ranges a thousandth of the store's span long, starting anywhere in it.
    const Nanos span = latest - earliest;
    const Nanos window = std::max<Nanos>(1, span / 1'000);
    for (int run = -1; run < opt.runs && row.ok; ++run) {
        gen::SplitMix64 rng(opt.seed + 77);  // the same ranges in every run and row
        std::uint64_t acc = 0;
        std::uint64_t blocks = 0;
        std::uint64_t found = 0;
        const Clock::time_point start = Clock::now();
        for (std::uint64_t q = 0; q < opt.queries; ++q) {
            const Nanos from = earliest + (span == 0 ? 0 : rng.below(span));
            const store::ScanResult result =
                reader.scan(from, from + window, [&acc](const Tick& tick) {
                    acc = fold(acc, tick);
                    return true;
                });
            blocks += result.blocks_read;
            found += result.ticks;
            row.ok = row.ok && result.ok;
        }
        const double ns = ns_since(start);
        if (run >= 0) {
            row.query_ns.push_back(ns / static_cast<double>(opt.queries));
        }
        row.blocks_per_query = static_cast<double>(blocks) / static_cast<double>(opt.queries);
        row.ticks_per_query = static_cast<double>(found) / static_cast<double>(opt.queries);
        static_cast<void>(acc);
    }
    return row;
}

// Whether the codec's functions exist yet.
template <class Codec>
bool written() {
    try {
        Codec codec;
        codec.reset();
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
    std::printf("tick_bench\n");
    std::printf("  ticks       %s over %s securities, from %s to %s\n",
                util::with_commas(r.ticks).c_str(), util::with_commas(r.securities).c_str(),
                util::format_time(r.earliest).c_str(), util::format_time(r.latest).c_str());
    std::printf("  source      %s\n", r.source.c_str());
    std::printf("  runs        %d per row; the median is shown\n", opt.runs);
    if (!opt.label.empty()) {
        std::printf("  label       %s\n", opt.label.c_str());
    }
    bench::print_environment(r.env, r.huge_bytes);

    std::printf("\n  %-8s %8s %11s %11s %9s %9s %10s %10s %10s\n", "codec", "block", "bytes/tick",
                "file/tick", "write ns", "read ns", "query us", "blocks/q", "ticks/q");
    for (const Row& row : r.rows) {
        const double ticks = static_cast<double>(r.ticks);
        std::printf("  %-8s %8u %11.2f %11.2f %9.1f %9.1f %10.1f %10.2f %10.0f%s\n",
                    row.codec.c_str(), row.block, static_cast<double>(row.payload_bytes) / ticks,
                    static_cast<double>(row.file_bytes) / ticks, summary(row.write_ns).median,
                    summary(row.read_ns).median, summary(row.query_ns).median / 1e3,
                    row.blocks_per_query, row.ticks_per_query, row.ok ? "" : "   WRONG TICKS");
    }
    std::printf(
        "\n  A query covers a thousandth of the time the store spans. It decodes every\n"
        "  block that range touches, whole: blocks/q times the block size is what it\n"
        "  read to hand over ticks/q.\n");
    for (const std::string& name : r.not_written) {
        std::printf("\n  codec '%s' is not written yet and was left out\n", name.c_str());
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
    std::fprintf(f, "{\n  \"benchmark\": \"tick_bench\",\n");
    std::fprintf(f, "  \"label\": \"%s\",\n", bench::json_escape(opt.label).c_str());
    bench::write_environment_json(f, r.env, r.huge_bytes);
    std::fprintf(f, "  \"source\": \"%s\",\n", bench::json_escape(r.source).c_str());
    std::fprintf(f, "  \"ticks\": %" PRIu64 ",\n", r.ticks);
    std::fprintf(f, "  \"securities\": %" PRIu64 ",\n", r.securities);
    std::fprintf(f, "  \"queries\": %" PRIu64 ",\n", opt.queries);
    std::fprintf(f, "  \"runs\": %d,\n", opt.runs);
    std::fprintf(f, "  \"rows\": [\n");
    for (std::size_t i = 0; i < r.rows.size(); ++i) {
        const Row& row = r.rows[i];
        const bench::Summary w = summary(row.write_ns);
        const bench::Summary d = summary(row.read_ns);
        const bench::Summary q = summary(row.query_ns);
        std::fprintf(f,
                     "    {\"codec\": \"%s\", \"ticks_per_block\": %u, \"blocks\": %" PRIu64
                     ", \"payload_bytes\": %" PRIu64 ", \"file_bytes\": %" PRIu64
                     ", \"write_ns_per_tick_median\": %.2f, \"write_ns_per_tick_min\": %.2f, "
                     "\"write_ns_per_tick_max\": %.2f, \"read_ns_per_tick_median\": %.2f, "
                     "\"read_ns_per_tick_min\": %.2f, \"read_ns_per_tick_max\": %.2f, "
                     "\"query_ns_median\": %.0f, \"blocks_per_query\": %.3f, "
                     "\"ticks_per_query\": %.1f, \"ok\": %s}%s\n",
                     bench::json_escape(row.codec).c_str(), row.block, row.blocks,
                     row.payload_bytes, row.file_bytes, w.median, w.min, w.max, d.median, d.min,
                     d.max, q.median, row.blocks_per_query, row.ticks_per_query,
                     row.ok ? "true" : "false", i + 1 < r.rows.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"not_written\": [");
    for (std::size_t i = 0; i < r.not_written.size(); ++i) {
        std::fprintf(f, "%s\"%s\"", i == 0 ? "" : ", ",
                     bench::json_escape(r.not_written[i]).c_str());
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
        if (arg == "--codec" && has_next) {
            opt.codec = argv[++i];
        } else if (arg == "--file" && has_next) {
            opt.file = argv[++i];
        } else if (arg == "--json" && has_next) {
            opt.json_path = argv[++i];
        } else if (arg == "--label" && has_next) {
            opt.label = argv[++i];
        } else if (arg == "--max-ticks" && has_value) {
            opt.max_ticks = value;
            ++i;
        } else if (arg == "--ticks" && has_value) {
            opt.ticks = value;
            ++i;
        } else if (arg == "--securities" && has_value) {
            opt.securities = static_cast<std::uint32_t>(std::min<std::uint64_t>(value, 65'535));
            ++i;
        } else if (arg == "--seed" && has_value) {
            opt.seed = value;
            ++i;
        } else if (arg == "--queries" && has_value) {
            opt.queries = value;
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
    if (opt.runs < 1 || opt.ticks == 0 || opt.max_ticks == 0 || opt.securities == 0 ||
        opt.queries == 0) {
        std::fprintf(stderr,
                     "error: --runs, --ticks, --max-ticks, --securities and --queries "
                     "must be at least 1\n");
        return 1;
    }
    if (!opt.codec.empty() && !store::with_codec(opt.codec, [](auto) {})) {
        std::fprintf(stderr, "error: no codec called '%s'. Try tick_store --list.\n",
                     opt.codec.c_str());
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

    std::vector<Tick> ticks;
    if (opt.file.empty()) {
        ticks = gen::market_ticks(opt.seed, opt.ticks, opt.securities);
        report.source = "generated (seed " + std::to_string(opt.seed) +
                        "): not a market, see obe/gen/tick_gen.hpp";
        report.warnings.emplace_back(
            "the ticks are generated: the compression ratio says nothing about a real feed");
    } else {
        const util::MappedFile file(opt.file);
        if (util::looks_gzipped(file.bytes())) {
            std::fprintf(stderr, "error: '%s' is gzip-compressed. Decompress it first.\n",
                         opt.file.c_str());
            return 1;
        }
        using Books = book::BookManager<book::OrderStore, book::PriceLevels, KeepTicks>;
        const auto books =
            std::make_unique<Books>(book::OrderStore{}, KeepTicks{&ticks, opt.max_ticks});
        feed::ItchParser parser(*books);
        const feed::ParseResult result = parser.parse(file.bytes());
        if (!result.ok()) {
            const std::string_view why = feed::to_string(result.status);
            std::fprintf(stderr,
                         "error: %.*s at byte offset %zu, after %" PRIu64 " good messages\n",
                         static_cast<int>(why.size()), why.data(), result.offset, result.messages);
            return 2;
        }
        report.source = opt.file;
        if (ticks.size() == opt.max_ticks) {
            report.source += "  (the first " + util::with_commas(opt.max_ticks) + " ticks)";
        }
    }
    if (ticks.empty()) {
        std::fprintf(stderr, "error: there are no ticks to measure\n");
        return 1;
    }

    std::uint64_t expected = 0;
    std::vector<bool> seen(std::size_t{1} << 16, false);
    Nanos earliest = ticks.front().timestamp;
    Nanos latest = earliest;
    for (const Tick& tick : ticks) {
        expected = fold(expected, tick);
        earliest = std::min(earliest, tick.timestamp);
        latest = std::max(latest, tick.timestamp);
        if (!seen[tick.locate]) {
            seen[tick.locate] = true;
            ++report.securities;
        }
    }
    report.ticks = ticks.size();
    report.earliest = earliest;
    report.latest = latest;

    bool ok = true;
    store::for_each_codec([&]<class Codec>(std::type_identity<Codec>) {
        if (!opt.codec.empty() && opt.codec != Codec::kName) {
            return;
        }
        if (!written<Codec>()) {
            report.not_written.emplace_back(Codec::kName);
            return;
        }
        for (const std::uint32_t block : {256U, 4'096U, 65'536U}) {
            Row row = measure<Codec>(opt, ticks, block, expected, earliest, latest);
            ok = ok && row.ok;
            for (const auto* runs : {&row.write_ns, &row.read_ns, &row.query_ns}) {
                if (summary(*runs).spread_percent() > 10.0) {
                    report.warnings.emplace_back("'" + row.codec + "' at " + std::to_string(block) +
                                                 " ticks a block varied by more than 10% "
                                                 "across runs");
                    break;
                }
            }
            report.rows.push_back(std::move(row));
        }
    });
    report.huge_bytes = util::process_huge_bytes();

    if (report.rows.empty()) {
        // The one codec that was asked for is the one that is not written.
        store::with_codec(opt.codec, []<class Codec>(std::type_identity<Codec>) {
            Codec codec;
            codec.reset();  // throws Unimplemented, with the name of what is missing
        });
        return 4;
    }
    print_text(report);
    if (!opt.json_path.empty() && !write_json(report)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", opt.json_path.c_str());
        return 1;
    }
    if (!ok) {
        std::printf("\nRESULT: a store did not give back the ticks it was given\n");
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
