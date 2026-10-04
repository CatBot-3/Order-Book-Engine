// book_replay: rebuild every book from an ITCH file and check the invariants.
//
//   book_replay <file>
//   book_replay --impl flat-store <file>     use another book implementation
//   book_replay --reserve 3000000 <file>     pre-size the order store
//   book_replay --hashes hashes.txt <file>   also write one line per security:
//                                            "<symbol> <locate> <updates> <hash>"
//   book_replay --list                       print the implementation names
//
// This is the run behind success criterion 2 (every invariant counter is zero
// at the end of a full day) and, through the hash, criterion 3: two book
// implementations that print the same hash published identical best-bid-and-
// offer streams. scripts/diff_books.sh runs it for several implementations and
// compares the per-security hashes.
//
// Exit status: 0 clean, 1 usage or I/O error, 2 the file is truncated or
// corrupt, 3 an invariant was violated, 4 the chosen implementation is not
// written yet.

#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "obe/book/bbo_hash.hpp"
#include "obe/book/book_manager.hpp"
#include "obe/book/implementations.hpp"
#include "obe/feed/parser.hpp"
#include "obe/util/format.hpp"
#include "obe/util/mapped_file.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;

struct Options {
    std::string path;
    std::string hashes_path;
    std::string impl = "reference";
    std::size_t reserve = 0;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: book_replay [--impl NAME] [--reserve N] [--hashes <output file>] "
                 "<uncompressed ITCH 5.0 file>\n"
                 "       book_replay --list\n");
    return status;
}

void list_implementations() {
    book::for_each_implementation([]<class Impl>(std::type_identity<Impl>) {
        std::printf("%-14.*s %.*s\n", static_cast<int>(Impl::kName.size()), Impl::kName.data(),
                    static_cast<int>(Impl::kDescription.size()), Impl::kDescription.data());
    });
}

void line(const char* label, std::uint64_t value) {
    std::printf("  %-28s %s\n", label, util::with_commas(value).c_str());
}

template <class Manager>
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

template <class Impl>
int replay(const Options& opt, std::span<const std::byte> buf) {
    using Manager = book::BookManager<typename Impl::Store, typename Impl::Levels, book::BboHasher>;

    Manager manager(book::make_store<typename Impl::Store>(opt.reserve));
    feed::ItchParser parser(manager);
    const feed::ParseResult result = parser.parse(buf);

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

    std::printf("replay (%.*s: %.*s)\n", static_cast<int>(Impl::kName.size()), Impl::kName.data(),
                static_cast<int>(Impl::kDescription.size()), Impl::kDescription.data());
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

    if (!opt.hashes_path.empty()) {
        if (!write_hashes(opt.hashes_path, manager)) {
            std::fprintf(stderr, "error: cannot write '%s'\n", opt.hashes_path.c_str());
            return 1;
        }
        std::printf("per-security hashes written to %s\n", opt.hashes_path.c_str());
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

int run(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = i + 1 < argc;
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--list") {
            list_implementations();
            return 0;
        }
        if (arg == "--hashes" && has_next) {
            opt.hashes_path = argv[++i];
        } else if (arg == "--impl" && has_next) {
            opt.impl = argv[++i];
        } else if (arg == "--reserve" && has_next) {
            const std::string_view text = argv[++i];
            const auto r = std::from_chars(text.data(), text.data() + text.size(), opt.reserve);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size()) {
                std::fprintf(stderr, "error: --reserve needs a non-negative integer\n");
                return 1;
            }
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

    const util::MappedFile file(opt.path);
    if (util::looks_gzipped(file.bytes())) {
        std::fprintf(stderr, "error: '%s' is gzip-compressed. Decompress it first.\n",
                     opt.path.c_str());
        return 1;
    }

    int status = 1;
    const bool known = book::with_implementation(
        opt.impl,
        [&]<class Impl>(std::type_identity<Impl>) { status = replay<Impl>(opt, file.bytes()); });
    if (!known) {
        std::fprintf(stderr, "error: no implementation called '%s'. Try --list.\n",
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
