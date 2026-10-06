// mm_sim: replay an ITCH file and simulate a market maker quoting one security.
//
//   mm_sim <file> --symbol AAPL [options]
//     --strategy NAME     fixed (default), join, as, none; --list prints them
//     --qty N             shares per quote (default 100)
//     --max-inventory N   stop adding to a position at this many shares (default 1000)
//     --half-spread N     fixed: distance of each quote from the mid, in ticks (default 1)
//     --gamma X --k X     as: risk aversion and fill-decay, per dollar (0.001, 200)
//     --sigma2 X          as: fix the variance (dollars^2 per second) instead of estimating it
//     --vol-half-life S   as: half-life of the variance estimate, seconds (default 60)
//     --max-tau S         as: longest horizon given to the formula, seconds (default 23400)
//     --tick N            price grid in ten-thousandths of a dollar (default 100, one cent)
//     --latency-us N      microseconds between a decision and its effect (default 0)
//     --from HH:MM[:SS]   quote only from this time of day...
//     --to HH:MM[:SS]     ...until this one (default: the whole file)
//     --rebate N          paid per share filled, ten-thousandths of a dollar; negative = fee
//     --fills FILE        write every fill as CSV
//     --json FILE         write the report as JSON
//
// On a real Nasdaq day use --from 09:30 --to 16:00: outside continuous
// trading the displayed book is not a market anyone can quote into.
//
// THIS IS A SIMULATION WITH STATED ASSUMPTIONS, NOT EVIDENCE OF PROFITABILITY.
// The recorded market never saw these quotes and did not react to them. What
// the numbers are good for is comparing strategies under the same assumptions
// and seeing adverse selection in the markouts. The assumptions are listed in
// obe/sim/simulator.hpp and in docs/design.md, and the report repeats them.
//
// Exit status: 0 clean, 1 usage or I/O error, 2 the file is truncated or
// corrupt, 3 the symbol is not in the file or the report contradicts itself,
// 4 the chosen strategy is not written yet.

#include <cerrno>
#include <charconv>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "obe/feed/parser.hpp"
#include "obe/sim/avellaneda_stoikov.hpp"
#include "obe/sim/simulator.hpp"
#include "obe/sim/strategy.hpp"
#include "obe/sim/types.hpp"
#include "obe/util/format.hpp"
#include "obe/util/mapped_file.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;

struct Options {
    std::string path;
    std::string strategy = "fixed";
    std::string fills_path;
    std::string json_path;
    sim::SimConfig sim;
    Qty qty = 100;
    std::int64_t max_inventory = 1'000;
    std::uint32_t half_spread_ticks = 1;
    sim::AsParams as;
    bool whole_file = true;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: mm_sim <uncompressed ITCH 5.0 file> --symbol SYMBOL\n"
                 "              [--strategy fixed|join|as|none] [--qty N] [--max-inventory N]\n"
                 "              [--half-spread TICKS] [--gamma X] [--k X] [--sigma2 X]\n"
                 "              [--vol-half-life SECONDS] [--max-tau SECONDS] [--tick N]\n"
                 "              [--latency-us N] [--from HH:MM[:SS]] [--to HH:MM[:SS]]\n"
                 "              [--rebate N] [--fills FILE] [--json FILE]\n"
                 "       mm_sim --list\n");
    return status;
}

void list_strategies() {
    std::printf(
        "fixed   quotes a fixed number of ticks either side of the mid (--half-spread)\n"
        "join    joins the real best bid and best offer, at the back of each queue\n"
        "as      Avellaneda-Stoikov: leans against inventory, widens with volatility\n"
        "none    never quotes: a plain replay, for checking the simulator itself\n");
}

template <class T>
bool parse_int(std::string_view text, T& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

bool parse_double(const char* text, double& out) {
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || errno != 0) {
        return false;
    }
    out = value;
    return true;
}

// "HH:MM", "HH:MM:SS" or "HH:MM:SS.fff" as nanoseconds since midnight.
bool parse_clock(std::string_view text, Nanos& out) {
    std::string full(text);
    if (full.size() == 5) {
        full += ":00";
    }
    const std::optional<Nanos> parsed = util::parse_time(full);
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

// --- Printing ------------------------------------------------------------------

[[nodiscard]] double dollars(std::int64_t price_units) {
    return static_cast<double>(price_units) / static_cast<double>(kPriceScale);
}
// A doubled amount (anything measured against the mid) in dollars.
[[nodiscard]] double dollars2(std::int64_t doubled) {
    return dollars(doubled) / 2.0;
}
// A doubled total over `shares` shares, as cents per share.
[[nodiscard]] double cents_per_share(std::int64_t doubled, std::uint64_t shares) {
    return shares == 0 ? 0.0 : dollars2(doubled) * 100.0 / static_cast<double>(shares);
}

void count(const char* label, std::uint64_t value) {
    std::printf("  %-44s %15s\n", label, util::with_commas(value).c_str());
}
void money(const char* label, double value) {
    std::printf("  %-44s %+15.2f\n", label, value);
}

[[nodiscard]] const char* reason_name(sim::FillReason reason) {
    switch (reason) {
        case sim::FillReason::QueueReached:
            return "queue";
        case sim::FillReason::TradedThrough:
            return "through";
        case sim::FillReason::CrossedByAdd:
            return "arrival";
    }
    return "?";
}

void print_report(const Options& opt, const std::string& described, const sim::SimReport& r) {
    std::printf("mm_sim: %s, strategy %s\n", opt.sim.symbol.c_str(), described.c_str());
    if (opt.whole_file) {
        std::printf("  window    the whole file\n");
    } else {
        std::printf("  window    %s to %s\n", util::format_time(opt.sim.start).substr(0, 8).c_str(),
                    util::format_time(opt.sim.end).substr(0, 8).c_str());
    }
    std::printf("  latency   %" PRIu64 " us between a decision and its effect\n",
                opt.sim.latency / 1'000);
    std::printf("  tick      %s    rebate %+.4f per share\n",
                util::format_price(opt.sim.tick).c_str(), dollars(opt.sim.rebate));

    std::printf("\nquoting\n");
    count("times the strategy was asked", r.decisions);
    count("quotes placed", r.placed);
    count("quotes cancelled", r.cancelled);
    count("refused: would have traded at once", r.rejected_crossing);
    count("refused: off the tick grid", r.rejected_off_tick);
    count("refused: bid not below ask", r.rejected_self_cross);
    count("refused: market closed or halted", r.rejected_closed);

    std::printf("\nfills\n");
    count("fills", r.fills);
    count("shares bought", r.bought);
    count("shares sold", r.sold);
    count("fills: an order behind ours traded", r.fills_queue);
    count("fills: a trade passed our price", r.fills_through);
    count("fills: an order arrived at our price", r.fills_crossed);
    count("displayed shares the market traded", r.market_executed);
    if (r.market_executed != 0) {
        std::printf("  %-44s %14.2f%%\n", "our shares as a share of that",
                    100.0 * static_cast<double>(r.bought + r.sold) /
                        static_cast<double>(r.market_executed));
    }

    std::printf("\nposition (shares)\n");
    std::printf("  %-44s %+15" PRId64 "\n", "at the end", r.inventory);
    std::printf("  %-44s %15" PRId64 "\n", "largest long", r.max_long);
    std::printf("  %-44s %15" PRId64 "\n", "largest short", r.max_short);
    std::printf("  %-44s %15.1f\n", "average size held, long or short", r.mean_abs_inventory);

    std::printf("\nmoney (dollars; what is still held is valued at the last mid, %.4f)\n",
                dollars2(r.mid2));
    money("profit", dollars2(r.pnl2));
    money("  = earned at the moment of each fill", dollars2(r.spread2));
    money("  + from holding the position afterwards", dollars2(r.inventory_pnl2));
    money("  + rebates", dollars(r.rebates));
    money("largest fall from an earlier high", -dollars2(r.max_drawdown2));

    const std::uint64_t traded = r.bought + r.sold;
    std::printf("\nper share filled (cents; + is in our favour)\n");
    std::printf("  %-44s %+15.3f\n", "distance from the mid at the fill",
                cents_per_share(r.spread2, traded));
    for (const sim::Markout& m : r.markouts) {
        char label[64];
        std::snprintf(label, sizeof(label), "distance from the mid %" PRIu64 " s later",
                      m.horizon / sim::kNanosPerSecond);
        std::printf("  %-44s %+15.3f   (%s shares)\n", label, cents_per_share(m.sum2, m.shares),
                    util::with_commas(m.shares).c_str());
    }
    std::printf(
        "  The first line is what quoting away from the mid earned. The others are\n"
        "  what was left of it after the price had moved: the difference is what\n"
        "  being filled just before the price moves against you costs.\n");

    std::printf(
        "\nassumptions\n"
        "  a. the recorded market does not react to our quotes, and the shares that\n"
        "     would have traded with us still trade with the recorded order too\n"
        "  b. hidden orders are ignored\n"
        "  c. queue position is exact for displayed orders (the feed names each one)\n"
        "  d. one fixed latency for new quotes and for cancels\n"
        "  e. one change in flight per side\n"
        "  f. quotes are passive: one that would trade on arrival is refused\n"
        "  This compares strategies under these assumptions. It is not an estimate\n"
        "  of what a strategy would earn.\n");
}

bool write_fills(const std::string& path, const std::vector<sim::Fill>& fills) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "time,side,price,shares,mid,reason\n");
    for (const sim::Fill& fill : fills) {
        std::fprintf(f, "%s,%c,%s,%u,%.5f,%s\n", util::format_time(fill.time).c_str(),
                     fill.side == Side::Buy ? 'B' : 'S', util::format_price(fill.price).c_str(),
                     fill.qty, dollars2(fill.mid2), reason_name(fill.reason));
    }
    return std::fclose(f) == 0;
}

bool write_json(const Options& opt, const std::string& described, const sim::SimReport& r) {
    std::FILE* f = std::fopen(opt.json_path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "{\n  \"tool\": \"mm_sim\",\n  \"simulation\": true,\n");
    std::fprintf(f, "  \"symbol\": \"%s\",\n  \"strategy\": \"%s\",\n", opt.sim.symbol.c_str(),
                 described.c_str());
    std::fprintf(f, "  \"latency_ns\": %" PRIu64 ",\n  \"tick\": %u,\n  \"rebate\": %" PRId64 ",\n",
                 opt.sim.latency, opt.sim.tick, opt.sim.rebate);
    std::fprintf(f,
                 "  \"decisions\": %" PRIu64 ",\n  \"placed\": %" PRIu64
                 ",\n  \"cancelled\": %" PRIu64 ",\n",
                 r.decisions, r.placed, r.cancelled);
    std::fprintf(f,
                 "  \"refused_crossing\": %" PRIu64 ",\n  \"refused_off_tick\": %" PRIu64
                 ",\n  \"refused_self_cross\": %" PRIu64 ",\n  \"refused_closed\": %" PRIu64 ",\n",
                 r.rejected_crossing, r.rejected_off_tick, r.rejected_self_cross,
                 r.rejected_closed);
    std::fprintf(
        f, "  \"fills\": %" PRIu64 ",\n  \"bought\": %" PRIu64 ",\n  \"sold\": %" PRIu64 ",\n",
        r.fills, r.bought, r.sold);
    std::fprintf(f,
                 "  \"fills_queue\": %" PRIu64 ",\n  \"fills_through\": %" PRIu64
                 ",\n  \"fills_arrival\": %" PRIu64 ",\n  \"market_executed\": %" PRIu64 ",\n",
                 r.fills_queue, r.fills_through, r.fills_crossed, r.market_executed);
    std::fprintf(f,
                 "  \"inventory\": %" PRId64 ",\n  \"max_long\": %" PRId64
                 ",\n  \"max_short\": %" PRId64 ",\n  \"mean_abs_inventory\": %.3f,\n",
                 r.inventory, r.max_long, r.max_short, r.mean_abs_inventory);
    std::fprintf(f,
                 "  \"last_mid\": %.5f,\n  \"profit\": %.4f,\n  \"earned_at_fills\": %.4f,\n"
                 "  \"from_holding\": %.4f,\n  \"rebates\": %.4f,\n  \"max_drawdown\": %.4f,\n",
                 dollars2(r.mid2), dollars2(r.pnl2), dollars2(r.spread2),
                 dollars2(r.inventory_pnl2), dollars(r.rebates), dollars2(r.max_drawdown2));
    std::fprintf(f, "  \"cents_per_share_at_fill\": %.5f,\n  \"markouts\": [\n",
                 cents_per_share(r.spread2, r.bought + r.sold));
    for (std::size_t i = 0; i < r.markouts.size(); ++i) {
        const sim::Markout& m = r.markouts[i];
        std::fprintf(f,
                     "    {\"seconds\": %" PRIu64 ", \"shares\": %" PRIu64
                     ", \"unmeasured_shares\": %" PRIu64 ", \"cents_per_share\": %.5f}%s\n",
                     m.horizon / sim::kNanosPerSecond, m.shares, m.unmeasured_shares,
                     cents_per_share(m.sum2, m.shares), i + 1 == r.markouts.size() ? "" : ",");
    }
    std::fprintf(f, "  ],\n  \"consistent\": %s\n}\n", r.consistent() ? "true" : "false");
    return std::fclose(f) == 0;
}

// --- The run ---------------------------------------------------------------------

template <sim::Strategy S>
int simulate(const Options& opt, std::span<const std::byte> buf, S strategy,
             const std::string& described) {
    sim::MarketMakingSim simulator(opt.sim, strategy);
    using Sim = sim::MarketMakingSim<S>;
    feed::ItchParser<Sim> parser(simulator);
    const feed::ParseResult result = parser.parse(buf);
    simulator.finish();

    if (!simulator.locate().has_value()) {
        std::fprintf(stderr, "error: no security called '%s' is listed in this file\n",
                     opt.sim.symbol.c_str());
        return 3;
    }
    const sim::SimReport report = simulator.report();
    print_report(opt, described, report);

    if (!opt.fills_path.empty()) {
        if (!write_fills(opt.fills_path, simulator.fills())) {
            std::fprintf(stderr, "error: cannot write '%s'\n", opt.fills_path.c_str());
            return 1;
        }
        std::printf("\nfills written to %s\n", opt.fills_path.c_str());
    }
    if (!opt.json_path.empty() && !write_json(opt, described, report)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", opt.json_path.c_str());
        return 1;
    }

    if (!result.ok()) {
        const std::string_view why = feed::to_string(result.status);
        std::fprintf(stderr, "error: %.*s at byte offset %zu, after %" PRIu64 " good messages\n",
                     static_cast<int>(why.size()), why.data(), result.offset, result.messages);
        return 2;
    }
    if (!report.consistent() || !simulator.book().counters().clean()) {
        std::printf("\nRESULT: the report contradicts itself, or the replay was not clean\n");
        return 3;
    }
    std::printf("\nRESULT: consistent (profit = earned at fills + from holding + rebates)\n");
    return 0;
}

int run(int argc, char** argv) {
    Options opt;
    std::uint64_t latency_us = 0;
    double half_life_seconds = 60.0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = i + 1 < argc;
        const auto bad = [&](const char* what) {
            std::fprintf(stderr, "error: %s needs %s\n", argv[i - 1], what);
            return 1;
        };
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--list") {
            list_strategies();
            return 0;
        }
        if (arg == "--symbol" && has_next) {
            opt.sim.symbol = argv[++i];
        } else if (arg == "--strategy" && has_next) {
            opt.strategy = argv[++i];
        } else if (arg == "--fills" && has_next) {
            opt.fills_path = argv[++i];
        } else if (arg == "--json" && has_next) {
            opt.json_path = argv[++i];
        } else if (arg == "--qty" && has_next) {
            if (!parse_int(argv[++i], opt.qty) || opt.qty == 0) {
                return bad("a positive number of shares");
            }
        } else if (arg == "--max-inventory" && has_next) {
            if (!parse_int(argv[++i], opt.max_inventory) || opt.max_inventory <= 0) {
                return bad("a positive number of shares");
            }
        } else if (arg == "--half-spread" && has_next) {
            if (!parse_int(argv[++i], opt.half_spread_ticks)) {
                return bad("a number of ticks");
            }
        } else if (arg == "--tick" && has_next) {
            if (!parse_int(argv[++i], opt.sim.tick) || opt.sim.tick == 0) {
                return bad("a positive number of price units");
            }
        } else if (arg == "--latency-us" && has_next) {
            if (!parse_int(argv[++i], latency_us)) {
                return bad("a number of microseconds");
            }
        } else if (arg == "--rebate" && has_next) {
            if (!parse_int(argv[++i], opt.sim.rebate)) {
                return bad("a whole number of price units");
            }
        } else if (arg == "--from" && has_next) {
            if (!parse_clock(argv[++i], opt.sim.start)) {
                return bad("a time of day, HH:MM or HH:MM:SS");
            }
            opt.whole_file = false;
        } else if (arg == "--to" && has_next) {
            if (!parse_clock(argv[++i], opt.sim.end)) {
                return bad("a time of day, HH:MM or HH:MM:SS");
            }
            opt.whole_file = false;
        } else if (arg == "--gamma" && has_next) {
            if (!parse_double(argv[++i], opt.as.gamma) || opt.as.gamma <= 0.0) {
                return bad("a positive number");
            }
        } else if (arg == "--k" && has_next) {
            if (!parse_double(argv[++i], opt.as.k) || opt.as.k <= 0.0) {
                return bad("a positive number");
            }
        } else if (arg == "--sigma2" && has_next) {
            if (!parse_double(argv[++i], opt.as.sigma2) || opt.as.sigma2 < 0.0) {
                return bad("a number that is not negative");
            }
        } else if (arg == "--vol-half-life" && has_next) {
            if (!parse_double(argv[++i], half_life_seconds) || half_life_seconds < 0.0) {
                return bad("a number of seconds");
            }
        } else if (arg == "--max-tau" && has_next) {
            if (!parse_double(argv[++i], opt.as.max_tau) || opt.as.max_tau < 0.0) {
                return bad("a number of seconds");
            }
        } else if (!arg.starts_with("-") && opt.path.empty()) {
            opt.path = arg;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (opt.path.empty() || opt.sim.symbol.empty()) {
        return usage(stderr, 1);
    }
    if (opt.sim.start >= opt.sim.end) {
        std::fprintf(stderr, "error: --from must be earlier than --to\n");
        return 1;
    }
    opt.sim.latency = latency_us * 1'000;
    opt.as.qty = opt.qty;
    opt.as.max_inventory = opt.max_inventory;
    opt.as.vol_half_life = static_cast<Nanos>(half_life_seconds * 1e9);

    const util::MappedFile file(opt.path);
    if (util::looks_gzipped(file.bytes())) {
        std::fprintf(stderr, "error: '%s' is gzip-compressed. Decompress it first.\n",
                     opt.path.c_str());
        return 1;
    }

    char text[160];
    if (opt.strategy == "fixed") {
        std::snprintf(text, sizeof(text), "fixed (%u tick%s either side of the mid, %u shares)",
                      opt.half_spread_ticks, opt.half_spread_ticks == 1 ? "" : "s", opt.qty);
        return simulate(opt, file.bytes(),
                        sim::FixedSpread{.half_spread = opt.half_spread_ticks * opt.sim.tick,
                                         .qty = opt.qty,
                                         .max_inventory = opt.max_inventory},
                        text);
    }
    if (opt.strategy == "join") {
        std::snprintf(text, sizeof(text), "join (the real best bid and offer, %u shares)", opt.qty);
        return simulate(opt, file.bytes(),
                        sim::JoinBest{.qty = opt.qty, .max_inventory = opt.max_inventory}, text);
    }
    if (opt.strategy == "as") {
        std::snprintf(text, sizeof(text), "as (Avellaneda-Stoikov, gamma %g, k %g, %u shares)",
                      opt.as.gamma, opt.as.k, opt.qty);
        return simulate(opt, file.bytes(), sim::AvellanedaStoikov(opt.as), text);
    }
    if (opt.strategy == "none") {
        return simulate(opt, file.bytes(), sim::NoQuotes{}, "none (never quotes)");
    }
    std::fprintf(stderr, "error: no strategy called '%s'. Try --list.\n", opt.strategy.c_str());
    return 1;
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
