// feed_profile: measure the facts about a feed that the phase 4 experiments
// turn on.
//
//   feed_profile <file>
//
// An optimization starts as a hypothesis about the data ("most updates land
// near the top of the book", "reference numbers are dense"). This tool turns
// each of those into a number before any code is written, so a log entry can
// say what was expected and why.
//
//   Section                    Informs
//   orders                     how large to pre-size the order table (exp. 1, 8)
//   order reference numbers    whether direct indexing is viable (exp. 2) and
//                              what the hash function will be fed (exp. 1)
//   price levels               how long a contiguous level array gets (exp. 3)
//   where updates land         whether "cheap near the best price" pays (exp. 3)
//   securities                 how cost scales with a watch list (exp. 9)
//
// It replays the file through the reference book, so it is slow next to
// replay_bench (it walks levels on every message) and its own speed means
// nothing.
//
// Exit status: 0 ok, 1 usage or I/O error, 2 the file is truncated or corrupt,
// 3 the profiler's own accounting does not add up.

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/feed/parser.hpp"
#include "obe/util/format.hpp"
#include "obe/util/latency_histogram.hpp"
#include "obe/util/mapped_file.hpp"

namespace {

using namespace obe;

using Manager = book::BookManager<book::OrderStore, book::PriceLevels>;

// Levels from the best price are counted exactly up to this many; anything
// deeper is reported as "64+".
constexpr std::size_t kDepthCap = 64;

class Profiler : public feed::HandlerBase {
 public:
    // --- orders ---
    std::uint64_t adds = 0;
    std::uint64_t peak_resting = 0;
    Nanos peak_time = 0;

    // --- reference numbers ---
    OrderId min_ref = ~OrderId{0};
    OrderId max_ref = 0;
    std::uint64_t increasing = 0;   // new reference above every earlier one
    std::uint64_t step_one = 0;     // exactly previous new reference + 1
    std::uint64_t step_small = 0;   // +2 to +15
    std::uint64_t step_medium = 0;  // +16 to +255
    std::uint64_t step_large = 0;   // +256 or more
    std::uint64_t step_back = 0;    // at or below the previous new reference

    // --- levels ---
    util::LatencyHistogram side_size;  // levels on the touched side, per update
    std::uint64_t most_levels = 0;
    Locate most_levels_locate = 0;
    Side most_levels_side = Side::Buy;
    std::uint64_t level_adds = 0;
    std::uint64_t level_adds_new = 0;  // the add created a level
    std::uint64_t level_removes = 0;
    std::uint64_t level_removes_emptied = 0;  // the removal erased a level

    // --- depth of update ---
    std::array<std::uint64_t, kDepthCap + 1> depth{};  // [kDepthCap] is "64+"
    std::uint64_t updates = 0;

    // --- securities ---
    std::vector<std::uint64_t> per_locate = std::vector<std::uint64_t>(Manager::kLocates, 0);
    std::uint64_t order_messages = 0;

    [[nodiscard]] const Manager& manager() const { return manager_; }

    void on_stock_directory(const feed::StockDirectory& m) { manager_.on_stock_directory(m); }
    void on_trading_action(const feed::TradingAction& m) { manager_.on_trading_action(m); }

    void on_add(const feed::AddOrder& m) {
        count(m.hdr);
        note_new_ref(m.order_ref);
        const std::size_t before = levels(m.hdr.locate, m.side).size();
        manager_.on_add(m);
        after_add(m.hdr, m.side, m.price, before);
        note_resting(m.hdr);
    }

    void on_execute(const feed::OrderExecuted& m) {
        count(m.hdr);
        reduce(m.hdr, m.order_ref, [&] { manager_.on_execute(m); });
    }
    void on_execute_with_price(const feed::OrderExecutedWithPrice& m) {
        count(m.hdr);
        reduce(m.hdr, m.order_ref, [&] { manager_.on_execute_with_price(m); });
    }
    void on_cancel(const feed::OrderCancel& m) {
        count(m.hdr);
        reduce(m.hdr, m.order_ref, [&] { manager_.on_cancel(m); });
    }
    void on_delete(const feed::OrderDelete& m) {
        count(m.hdr);
        reduce(m.hdr, m.order_ref, [&] { manager_.on_delete(m); });
    }

    void on_replace(const feed::OrderReplace& m) {
        count(m.hdr);
        note_new_ref(m.new_order_ref);
        const book::OrderRecord* old = manager_.orders().find(m.orig_order_ref);
        if (old == nullptr) {
            manager_.on_replace(m);  // the book counts it as an unknown order
            return;
        }
        // A replace is a removal at the old price and an add at the new one.
        // Copy what is needed first: `old` does not survive the replace.
        const Side side = old->side;
        const Price old_price = old->price;
        const Qty old_qty = old->qty;
        const book::PriceLevels& lv = levels(m.hdr.locate, side);

        const Found old_level = find_level(lv, old_price);
        record_depth(old_level.rank);
        side_size.record(lv.size());

        // A replace that keeps its price leaves the level where it was, even
        // if this order was alone on it, so it neither empties nor creates.
        const bool moves = m.price != old_price;
        const bool empties = moves && old_level.qty == old_qty;
        const bool creates = moves && m.shares != 0 && find_level(lv, m.price).rank == kNotFound;

        manager_.on_replace(m);

        ++level_removes;
        ++level_adds;
        level_removes_emptied += empties ? 1U : 0U;
        level_adds_new += creates ? 1U : 0U;
        record_depth(find_level(lv, m.price).rank);
        side_size.record(lv.size());
        note_side_size(m.hdr.locate, side, lv.size());
        note_resting(m.hdr);
    }

 private:
    static constexpr std::size_t kNotFound = ~std::size_t{0};

    [[nodiscard]] const book::PriceLevels& levels(Locate locate, Side side) const {
        return manager_.book(locate).side(side);
    }

    struct Found {
        std::size_t rank = kNotFound;  // levels better than this one; 0 is the best
        std::uint64_t qty = 0;
    };

    // The level at `price` on this side: how many levels are better than it,
    // and how many shares it holds. rank is kNotFound if there is no such
    // level. This walks from the best price, so it costs as much as the level
    // is deep. That is acceptable for an offline tool and would not be for the
    // book itself.
    [[nodiscard]] static Found find_level(const book::PriceLevels& side, Price price) {
        Found out;
        std::size_t rank = 0;
        side.for_each([&](const book::Level& level) {
            if (level.price == price) {
                out.rank = rank;
                out.qty = level.qty;
                return false;
            }
            ++rank;
            return true;
        });
        return out;
    }

    void count(const feed::Header& hdr) {
        ++order_messages;
        ++per_locate[hdr.locate];
    }

    void note_new_ref(OrderId ref) {
        if (adds != 0) {
            if (ref > max_ref) {
                ++increasing;
            }
            if (ref <= previous_ref_) {
                ++step_back;
            } else {
                const OrderId step = ref - previous_ref_;
                if (step == 1) {
                    ++step_one;
                } else if (step < 16) {
                    ++step_small;
                } else if (step < 256) {
                    ++step_medium;
                } else {
                    ++step_large;
                }
            }
        }
        ++adds;
        previous_ref_ = ref;
        min_ref = std::min(min_ref, ref);
        max_ref = std::max(max_ref, ref);
    }

    void note_resting(const feed::Header& hdr) {
        const std::uint64_t resting = manager_.orders().size();
        if (resting > peak_resting) {
            peak_resting = resting;
            peak_time = hdr.timestamp;
        }
    }

    void note_side_size(Locate locate, Side side, std::size_t size) {
        if (size > most_levels) {
            most_levels = size;
            most_levels_locate = locate;
            most_levels_side = side;
        }
    }

    void record_depth(std::size_t rank) {
        if (rank == kNotFound) {
            return;
        }
        ++depth[rank < kDepthCap ? rank : kDepthCap];
        ++updates;
    }

    void after_add(const feed::Header& hdr, Side side, Price price, std::size_t before) {
        const book::PriceLevels& lv = levels(hdr.locate, side);
        const std::size_t after = lv.size();
        ++level_adds;
        if (after > before) {
            ++level_adds_new;
        }
        record_depth(find_level(lv, price).rank);
        side_size.record(after);
        note_side_size(hdr.locate, side, after);
    }

    // Record where a removal is about to land. Called before the book changes,
    // while the level still exists.
    void before_remove(const feed::Header& hdr, Side side, Price price) {
        const book::PriceLevels& lv = levels(hdr.locate, side);
        record_depth(find_level(lv, price).rank);
        side_size.record(lv.size());
    }

    template <class Apply>
    void reduce(const feed::Header& hdr, OrderId ref, Apply&& apply) {
        const book::OrderRecord* rec = manager_.orders().find(ref);
        if (rec == nullptr) {
            apply();  // the book counts it as an unknown order
            return;
        }
        const Side side = rec->side;
        before_remove(hdr, side, rec->price);
        const std::size_t before = levels(hdr.locate, side).size();
        apply();
        ++level_removes;
        if (levels(hdr.locate, side).size() < before) {
            ++level_removes_emptied;
        }
    }

    Manager manager_;
    OrderId previous_ref_ = 0;
};

double percent(std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

void row(const char* label, const std::string& value) {
    std::printf("  %-36s %s\n", label, value.c_str());
}

void row_count(const char* label, std::uint64_t value) {
    row(label, util::with_commas(value));
}

void row_percent(const char* label, std::uint64_t part, std::uint64_t whole) {
    std::array<char, 64> buf{};
    std::snprintf(buf.data(), buf.size(), "%6.2f%%  (%s)", percent(part, whole),
                  util::with_commas(part).c_str());
    row(label, buf.data());
}

void report(const Profiler& p, std::uint64_t messages) {
    const Manager& m = p.manager();

    std::printf("orders\n");
    row_count("messages in the file", messages);
    row_count("order messages (A F E C X D U)", p.order_messages);
    row_count("orders added (A, F, and U's new one)", p.adds);
    row_count("most resting at once", p.peak_resting);
    row("  reached at", util::format_time(p.peak_time));
    row_count("resting at the end", m.orders().size());
    std::printf("  -> an order table needs room for at least %s orders; pass that to --reserve\n",
                util::with_commas(p.peak_resting).c_str());

    std::printf("\norder reference numbers\n");
    if (p.adds != 0) {
        const std::uint64_t span = p.max_ref - p.min_ref + 1;
        row_count("smallest", p.min_ref);
        row_count("largest", p.max_ref);
        row_count("span (largest - smallest + 1)", span);
        std::array<char, 64> buf{};
        std::snprintf(buf.data(), buf.size(), "%.4f%%", percent(p.adds, span));
        row("density (orders added / span)", buf.data());
        std::printf(
            "  -> an array indexed by reference number would need %s slots for a peak of "
            "%s orders\n",
            util::with_commas(span).c_str(), util::with_commas(p.peak_resting).c_str());
        const std::uint64_t steps = p.adds - 1;
        row_percent("new reference above all earlier ones", p.increasing, steps);
        std::printf("  step from the previous new reference:\n");
        row_percent("  +1", p.step_one, steps);
        row_percent("  +2 to +15", p.step_small, steps);
        row_percent("  +16 to +255", p.step_medium, steps);
        row_percent("  +256 or more", p.step_large, steps);
        row_percent("  not above the previous one", p.step_back, steps);
    }

    std::printf("\nprice levels on a side, at the moment it is updated\n");
    row_count("median", p.side_size.percentile(50));
    row_count("90th percentile", p.side_size.percentile(90));
    row_count("99th percentile", p.side_size.percentile(99));
    row_count("largest", p.side_size.max());
    if (p.most_levels != 0) {
        const std::string where = std::string(m.symbol(p.most_levels_locate)) +
                                  (p.most_levels_side == Side::Buy ? " bids" : " asks");
        row("  largest side", where);
    }
    row_percent("adds that created a level", p.level_adds_new, p.level_adds);
    row_percent("removals that emptied a level", p.level_removes_emptied, p.level_removes);

    std::printf("\nwhere updates land (levels from the best price; 0 is the best)\n");
    std::uint64_t running = 0;
    const auto band = [&](const char* label, std::size_t from, std::size_t to) {
        std::uint64_t n = 0;
        for (std::size_t i = from; i <= to; ++i) {
            n += p.depth[i];
        }
        running += n;
        std::printf("  %-10s %6.2f%%   cumulative %6.2f%%\n", label, percent(n, p.updates),
                    percent(running, p.updates));
    };
    band("0", 0, 0);
    band("1", 1, 1);
    band("2", 2, 2);
    band("3", 3, 3);
    band("4", 4, 4);
    band("5 to 9", 5, 9);
    band("10 to 19", 10, 19);
    band("20 to 63", 20, 63);
    band("64+", kDepthCap, kDepthCap);

    std::printf("\nsecurities\n");
    std::vector<std::pair<std::uint64_t, Locate>> ranked;
    for (std::size_t i = 0; i < Manager::kLocates; ++i) {
        if (p.per_locate[i] != 0) {
            ranked.emplace_back(p.per_locate[i], static_cast<Locate>(i));
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    row_count("with at least one order message", ranked.size());
    std::uint64_t cumulative = 0;
    std::size_t next_mark = 1;
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        cumulative += ranked[i].first;
        if (i + 1 == next_mark || i + 1 == ranked.size()) {
            std::array<char, 48> label{};
            std::snprintf(label.data(), label.size(), "busiest %zu carry", i + 1);
            row_percent(label.data(), cumulative, p.order_messages);
            next_mark *= 10;
        }
    }
    std::printf("  busiest:");
    for (std::size_t i = 0; i < ranked.size() && i < 8; ++i) {
        const std::string_view symbol = m.symbol(ranked[i].second);
        std::printf(" %.*s %.1f%%", static_cast<int>(symbol.size()), symbol.data(),
                    percent(ranked[i].first, p.order_messages));
    }
    std::printf("\n");

    std::printf("\nrecord sizes in this build\n");
    row_count("sizeof(OrderRecord)", sizeof(book::OrderRecord));
    row_count("sizeof(Level)", sizeof(book::Level));
    row_count("sizeof(Book)", sizeof(Manager::BookType));

    if (!m.counters().clean()) {
        std::printf(
            "\nwarning: the book's invariant counters are not all zero; run book_replay.\n");
    }
}

// The profiler counts levels created and levels emptied on its own. Their
// difference must be the number of levels the book ends with. If it is not,
// one of the figures above was computed wrongly and none should be trusted.
bool self_check(const Profiler& p) {
    const std::uint64_t levels = p.manager().audit().levels;
    const bool consistent = p.level_adds_new >= p.level_removes_emptied &&
                            p.level_adds_new - p.level_removes_emptied == levels;
    if (consistent) {
        std::printf(
            "\nself-check: levels created minus levels emptied equals the %s levels in the "
            "book: ok\n",
            util::with_commas(levels).c_str());
    } else {
        std::printf("\nself-check FAILED: created %s, emptied %s, but the book holds %s levels\n",
                    util::with_commas(p.level_adds_new).c_str(),
                    util::with_commas(p.level_removes_emptied).c_str(),
                    util::with_commas(levels).c_str());
    }
    return consistent;
}

int run(int argc, char** argv) {
    std::string path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::printf("usage: feed_profile <uncompressed ITCH 5.0 file>\n");
            return 0;
        }
        if (!arg.starts_with("-") && path.empty()) {
            path = arg;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return 1;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: feed_profile <uncompressed ITCH 5.0 file>\n");
        return 1;
    }

    const util::MappedFile file(path);
    if (util::looks_gzipped(file.bytes())) {
        std::fprintf(stderr, "error: '%s' is gzip-compressed. Decompress it first.\n",
                     path.c_str());
        return 1;
    }

    const auto profiler = std::make_unique<Profiler>();
    feed::ItchParser parser(*profiler);
    const feed::ParseResult result = parser.parse(file.bytes());
    report(*profiler, result.messages);
    const bool consistent = self_check(*profiler);

    if (!result.ok()) {
        const std::string_view why = feed::to_string(result.status);
        std::fprintf(stderr, "error: %.*s at byte offset %zu, after %" PRIu64 " good messages\n",
                     static_cast<int>(why.size()), why.data(), result.offset, result.messages);
        return 2;
    }
    return consistent ? 0 : 3;
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
