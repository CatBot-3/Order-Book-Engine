// book_replay: rebuild every book from an ITCH file and check the invariants.
//
//   book_replay <file>
//   book_replay --hashes hashes.txt <file>   also write one line per security:
//                                            "<symbol> <locate> <updates> <hash>"
//
// This is the run behind success criterion 2 (every invariant counter is zero
// at the end of a full day) and, through the hash, criterion 3: two book
// implementations that print the same hash published identical best-bid-and-
// offer streams.
//
// Exit status: 0 clean, 1 usage or I/O error, 2 the file is truncated or
// corrupt, 3 an invariant was violated, 4 the book containers are not written
// yet.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

#include "obe/book/bbo_hash.hpp"
#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/feed/parser.hpp"
#include "obe/util/format.hpp"
#include "obe/util/mapped_file.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;

using Manager = book::BookManager<book::OrderStore, book::PriceLevels, book::BboHasher>;

int usage(std::FILE* to, int status) {
    std::fprintf(to, "usage: book_replay [--hashes <output file>] <uncompressed ITCH 5.0 file>\n");
    return status;
}

void line(const char* label, std::uint64_t value) {
    std::printf("  %-28s %s\n", label, util::with_commas(value).c_str());
}

bool write_hashes(const std::string& path, const Manager& manager) {
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
        return false;
    }
    const book::BboHasher& hasher = manager.listener();
    for (std::size_t i = 0; i < Manager::kLocates; ++i) {
        const auto locate = static_cast<Locate>(i);
        if (hasher.events(locate) == 0) {
            continue;
        }
        const std::string_view symbol = manager.symbol(locate);
        std::fprintf(out, "%.*s %u %" PRIu64 " %016" PRIx64 "\n", static_cast<int>(symbol.size()),
                     symbol.data(), static_cast<unsigned>(locate), hasher.events(locate),
                     hasher.hash(locate));
    }
    return std::fclose(out) == 0;
}

int run(int argc, char** argv) {
    std::string path;
    std::string hashes_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--hashes" && i + 1 < argc) {
            hashes_path = argv[++i];
        } else if (!arg.starts_with("-") && path.empty()) {
            path = arg;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (path.empty()) {
        return usage(stderr, 1);
    }

    const util::MappedFile file(path);
    if (util::looks_gzipped(file.bytes())) {
        std::fprintf(stderr, "error: '%s' is gzip-compressed. Decompress it first.\n",
                     path.c_str());
        return 1;
    }

    Manager manager;
    feed::ItchParser parser(manager);
    const feed::ParseResult result = parser.parse(file.bytes());

    const book::Counters& counters = manager.counters();
    const book::Stats& stats = manager.stats();
    const book::Audit audit = manager.audit();

    std::uint64_t executed = 0;
    std::uint64_t securities = 0;
    for (std::size_t i = 0; i < Manager::kLocates; ++i) {
        const auto locate = static_cast<Locate>(i);
        executed += manager.book(locate).executed_shares();
        securities += manager.listener().events(locate) != 0 ? 1U : 0U;
    }

    std::printf("replay\n");
    line("messages", result.messages);
    line("securities with a book", securities);
    line("best bid/offer updates", stats.bbo_updates);
    line("shares executed (displayed)", executed);
    line("orders resting at the end", audit.open_orders);
    line("price levels at the end", audit.levels);

    std::printf("\ninvariants (each must be 0)\n");
    line("unknown order reference", counters.unknown_order);
    line("negative remaining shares", counters.overfill);
    line("duplicate order reference", counters.duplicate_order);
    line("zero-share message", counters.zero_shares);
    line("level/order disagreement", counters.level_mismatch);
    line("empty level left behind", audit.empty_levels);
    const std::uint64_t share_gap = audit.level_shares > audit.open_shares
                                        ? audit.level_shares - audit.open_shares
                                        : audit.open_shares - audit.level_shares;
    line("level shares vs order shares", share_gap);

    std::printf("\nlocked or crossed best bid/offer (statistics, see docs/design.md)\n");
    line("locked, state 'T'", stats.locked_while_trading);
    line("locked, any other state", stats.locked_while_not_trading);
    line("crossed, state 'T'", stats.crossed_while_trading);
    line("crossed, any other state", stats.crossed_while_not_trading);

    std::printf("\nbest bid/offer stream hash: %016" PRIx64 "\n", manager.listener().combined());

    if (!hashes_path.empty()) {
        if (!write_hashes(hashes_path, manager)) {
            std::fprintf(stderr, "error: cannot write '%s'\n", hashes_path.c_str());
            return 1;
        }
        std::printf("per-security hashes written to %s\n", hashes_path.c_str());
    }

    if (!result.ok()) {
        const std::string_view why = feed::to_string(result.status);
        std::fprintf(stderr, "error: %.*s at byte offset %zu, after %" PRIu64 " good messages\n",
                     static_cast<int>(why.size()), why.data(), result.offset, result.messages);
        return 2;
    }
    if (!counters.clean() || !audit.clean()) {
        std::printf("\nRESULT: invariant violations found\n");
        return 3;
    }
    std::printf("\nRESULT: clean\n");
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
