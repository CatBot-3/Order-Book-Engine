// flow_gen: run seeded order flow through a matching engine and write the
// market data it publishes as an ITCH 5.0 file.
//
//   flow_gen [options] [<output file>]
//
// The output is in the layout of Nasdaq's sample files, so itch_stats,
// book_replay, book_view and replay_bench read it like a real day. Unlike
// itch_synth, the book in it is one a real matching engine produced: trades
// take the best price and the oldest order, and the two sides never cross.
// See obe/gen/order_flow.hpp for what the flow is and is not.
//
// Unless --no-verify is given, the run ends with the round trip (success
// criterion 7): every byte written is also parsed back through the feed
// handler into a BookManager, and that book must equal the engine's own.
//
// The two hashes it prints identify everything the engine said. Two engines
// that print the same pair for the same options behaved identically;
// scripts/diff_engines.sh makes that comparison.
//
// Exit status: 0 clean, 1 usage or I/O error, 2 the round trip failed,
// 4 the chosen engine is not written yet.

#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/engine/engines.hpp"
#include "obe/engine/feed_writer.hpp"
#include "obe/engine/output_hash.hpp"
#include "obe/engine/round_trip.hpp"
#include "obe/feed/parser.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/util/format.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;

struct Options {
    gen::FlowConfig flow;
    std::uint64_t commands = 1'000'000;
    std::string engine = "reference";
    std::string output;
    bool verify = true;
};

int usage(std::FILE* to, int status) {
    std::fprintf(
        to,
        "usage: flow_gen [options] [<output file>]\n"
        "       flow_gen --list\n"
        "  --seed N       PRNG seed (default 1). The same options give the same bytes.\n"
        "  --commands N   requests sent to the engine (default 1000000)\n"
        "  --symbols N    number of securities (default 16)\n"
        "  --owners N     number of participants (default 8)\n"
        "  --live N       resting orders the flow hovers around (default 2000)\n"
        "  --bad N        deliberately invalid requests per million (default 500)\n"
        "  --icebergs N   limit day orders per million given a display size (default 0)\n"
        "  --post-only N  limit day orders per million that are post-only (default 0)\n"
        "  --self-match N orders per million asking for self-match prevention (default 0)\n"
        "  --engine NAME  which matching engine (default reference)\n"
        "  --no-verify    skip the round trip through the feed handler\n"
        "With no output file the run is made and summarised, and nothing is written.\n");
    return status;
}

void list_engines() {
    engine::for_each_engine([]<class Impl>(std::type_identity<Impl>) {
        std::printf("%-10.*s %.*s\n", static_cast<int>(Impl::kName.size()), Impl::kName.data(),
                    static_cast<int>(Impl::kDescription.size()), Impl::kDescription.data());
    });
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

void line(const char* label, std::uint64_t value) {
    std::printf("  %-26s %s\n", label, util::with_commas(value).c_str());
}

template <class Impl>
int run_engine(const Options& opt) {
    using Reports = engine::TeeReports<gen::OrderFlow, engine::ReportHasher>;
    using Engine = typename Impl::template Engine<Reports, engine::ItchFeedWriter>;
    using Mirror = book::BookManager<book::OrderStore, book::PriceLevels>;

    gen::OrderFlow flow(opt.flow);
    engine::ReportHasher report_hash;
    Reports reports(flow, report_hash);
    engine::ItchFeedWriter writer;
    // The engine holds a book for every possible locate; keep it off the stack.
    const auto engine = std::make_unique<Engine>(reports, writer);

    std::ofstream file;
    if (!opt.output.empty()) {
        file.open(opt.output, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::fprintf(stderr, "error: cannot open '%s' for writing\n", opt.output.c_str());
            return 1;
        }
    }

    // The second book, built from nothing but the published bytes.
    const std::unique_ptr<Mirror> mirror = opt.verify ? std::make_unique<Mirror>() : nullptr;
    engine::ByteHasher feed_hash;
    bool parse_ok = true;

    // Hash, parse back and write out what has been published so far.
    const auto flush = [&] {
        const std::vector<std::byte> chunk = writer.take();
        feed_hash.update(chunk);
        if (mirror != nullptr) {
            feed::ItchParser parser(*mirror);
            parse_ok = parse_ok && parser.parse(chunk).ok();
        }
        if (file.is_open()) {
            file.write(reinterpret_cast<const char*>(chunk.data()),
                       static_cast<std::streamsize>(chunk.size()));
        }
    };

    constexpr std::size_t kFlushAt = std::size_t{4} << 20;

    writer.system_event('O', flow.now());
    flow.open(*engine);
    writer.system_event('S', flow.now());
    writer.system_event('Q', flow.now());
    for (std::uint64_t i = 0; i < opt.commands; ++i) {
        gen::apply(*engine, flow.next());
        if (writer.bytes().size() >= kFlushAt) {
            flush();
        }
    }
    writer.system_event('M', flow.now());
    writer.system_event('E', flow.now());
    writer.system_event('C', flow.now());
    flush();

    if (file.is_open()) {
        file.close();
        if (!file) {
            std::fprintf(stderr, "error: cannot write '%s'\n", opt.output.c_str());
            return 1;
        }
    }

    const engine::EngineStats& stats = engine->stats();
    std::printf("flow_gen (%.*s: %.*s)\n", static_cast<int>(Impl::kName.size()), Impl::kName.data(),
                static_cast<int>(Impl::kDescription.size()), Impl::kDescription.data());
    line("seed", opt.flow.seed);
    line("symbols", flow.config().symbols);
    line("requests", opt.commands);
    line("orders accepted", stats.accepted);
    line("requests rejected", stats.rejected);
    line("cancels", stats.cancels);
    line("replaces", stats.replaces);
    line("trades", stats.trades);
    line("shares traded", stats.traded_shares);
    line("shares left unfilled", stats.unfilled_shares);
    line("self-matches prevented", stats.self_matches);
    line("orders resting at the end", engine->open_orders());

    std::printf("\nmarket data\n");
    line("messages", writer.messages());
    line("bytes", writer.total_bytes());
    line("add (A)", writer.count('A'));
    line("executed (E)", writer.count('E'));
    line("cancel (X)", writer.count('X'));
    line("delete (D)", writer.count('D'));
    line("replace (U)", writer.count('U'));
    line("reports to owners", report_hash.reports());

    std::printf("\nfeed hash:   %016" PRIx64 "\n", feed_hash.hash());
    std::printf("report hash: %016" PRIx64 "\n", report_hash.hash());
    if (!opt.output.empty()) {
        std::printf("written to %s\n", opt.output.c_str());
    }

    if (flow.live() != engine->open_orders()) {
        // The generator follows the book through the reports alone. If the two
        // counts differ, a report was wrong or missing.
        std::printf(
            "\nRESULT: the reports do not account for the book (%zu orders by the "
            "reports, %zu resting)\n",
            flow.live(), engine->open_orders());
        return 2;
    }

    if (mirror == nullptr) {
        std::printf("\nround trip: skipped\n");
        return 0;
    }
    const engine::DepthComparison depth = engine::compare_depth(*engine, *mirror);
    const book::Counters& counters = mirror->counters();
    const book::Stats& book_stats = mirror->stats();
    const book::Audit audit = mirror->audit();
    const std::uint64_t locked_or_crossed =
        book_stats.locked_while_trading + book_stats.locked_while_not_trading +
        book_stats.crossed_while_trading + book_stats.crossed_while_not_trading;

    std::printf("\nround trip through the feed handler\n");
    line("securities with a book", depth.securities);
    line("levels that match", depth.levels);
    line("sides that differ", depth.mismatched_sides);
    line("orders in the rebuilt book", audit.open_orders);
    line("locked or crossed updates", locked_or_crossed);

    const bool ok = parse_ok && depth.ok() && counters.clean() && audit.clean() &&
                    audit.open_orders == engine->open_orders() && locked_or_crossed == 0;
    if (!ok) {
        if (!parse_ok) {
            std::printf("  the published bytes did not parse\n");
        }
        if (!counters.clean() || !audit.clean()) {
            std::printf("  the rebuilt book broke an invariant\n");
        }
        if (!depth.ok()) {
            std::printf("  first difference: locate %u, %s side\n",
                        static_cast<unsigned>(depth.bad_locate),
                        depth.bad_side == Side::Buy ? "buy" : "sell");
        }
        std::printf("\nRESULT: round trip FAILED\n");
        return 2;
    }
    std::printf("\nRESULT: round trip ok\n");
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
        if (arg == "--list") {
            list_engines();
            return 0;
        }
        if (arg == "--no-verify") {
            opt.verify = false;
        } else if (arg == "--engine" && has_next) {
            opt.engine = argv[++i];
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
        } else if (arg == "--bad" && has_value) {
            opt.flow.bad_per_million = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--icebergs" && has_value) {
            opt.flow.iceberg_per_million = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--post-only" && has_value) {
            opt.flow.post_only_per_million = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--self-match" && has_value) {
            opt.flow.self_match_per_million = static_cast<std::uint32_t>(value);
            ++i;
        } else if (!arg.starts_with("-") && opt.output.empty()) {
            opt.output = arg;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }

    int status = 1;
    const bool known = engine::with_engine(
        opt.engine, [&]<class Impl>(std::type_identity<Impl>) { status = run_engine<Impl>(opt); });
    if (!known) {
        std::fprintf(stderr, "error: no engine called '%s'. Try --list.\n", opt.engine.c_str());
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
