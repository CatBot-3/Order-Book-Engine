// book_view: print the top of one security's book at a point in time.
//
//   book_view <file> <symbol>                      the book at the end of the file
//   book_view <file> <symbol> --at 10:15:00        the book as of 10:15:00.000000000
//   book_view <file> <symbol> --at 09:30:00.5 --depth 20
//
// "As of T" means after every message with a timestamp <= T has been applied.
//
// Only messages for the requested security are applied. That is sound because
// every order message carries its security's locate in the header, so an order
// that belongs to another security can never be referenced by this one's
// messages.
//
// Exit status: 0 ok, 1 usage or I/O error, 2 the file is truncated or corrupt,
// 4 the book containers are not written yet.

#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/feed/parser.hpp"
#include "obe/util/format.hpp"
#include "obe/util/mapped_file.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;

using Manager = book::BookManager<book::OrderStore, book::PriceLevels>;

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: book_view <uncompressed ITCH 5.0 file> <symbol> [--at HH:MM:SS[.frac]] "
                 "[--depth N]\n");
    return status;
}

std::vector<book::Level> top(const book::PriceLevels& levels, std::size_t depth) {
    std::vector<book::Level> out;
    levels.for_each([&](const book::Level& level) {
        out.push_back(level);
        return out.size() < depth;
    });
    return out;
}

int run(int argc, char** argv) {
    std::string path;
    std::string symbol;
    std::optional<Nanos> at;
    std::size_t depth = 10;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--at" && i + 1 < argc) {
            at = util::parse_time(argv[++i]);
            if (!at) {
                std::fprintf(stderr, "error: cannot read '%s' as HH:MM:SS[.frac]\n", argv[i]);
                return 1;
            }
        } else if (arg == "--depth" && i + 1 < argc) {
            const std::string_view text = argv[++i];
            const auto r = std::from_chars(text.data(), text.data() + text.size(), depth);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || depth == 0) {
                std::fprintf(stderr, "error: --depth needs a positive integer\n");
                return 1;
            }
        } else if (!arg.starts_with("-") && path.empty()) {
            path = arg;
        } else if (!arg.starts_with("-") && symbol.empty()) {
            symbol = arg;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (path.empty() || symbol.empty()) {
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
    feed::FrameReader reader(file.bytes());
    feed::Frame frame;
    feed::ParseStatus status = feed::ParseStatus::Ok;
    std::optional<Locate> locate;
    std::uint64_t applied = 0;
    Nanos last_applied = 0;

    while (!reader.done()) {
        const std::size_t offset = reader.offset();
        status = reader.next(frame);
        if (status != feed::ParseStatus::Ok) {
            std::fprintf(stderr, "error: %s at byte offset %zu\n",
                         std::string(feed::to_string(status)).c_str(), offset);
            return 2;
        }
        if (frame.size < 11) {
            std::fprintf(stderr, "error: record too short at byte offset %zu\n", offset);
            return 2;
        }
        if (at && feed::peek_timestamp(frame) > *at) {
            break;
        }
        // Stock Directory messages are always applied: they are how the symbol
        // is turned into a locate. Everything else is applied only for the
        // requested security.
        const bool wanted =
            frame.type() == 'R' || (locate.has_value() && feed::peek_locate(frame) == *locate);
        if (!wanted) {
            continue;
        }
        status = parser.dispatch(frame);
        if (status != feed::ParseStatus::Ok) {
            std::fprintf(stderr, "error: %s at byte offset %zu\n",
                         std::string(feed::to_string(status)).c_str(), offset);
            return 2;
        }
        if (frame.type() == 'R') {
            // dispatch() accepted the frame, so it is a full Stock Directory.
            if (feed::decode<feed::StockDirectory>(frame.data).stock.view() == symbol) {
                locate = feed::peek_locate(frame);
            }
        } else {
            ++applied;
            last_applied = feed::peek_timestamp(frame);
        }
    }

    if (!locate) {
        std::fprintf(stderr, "error: no Stock Directory entry for '%s' before the cut-off\n",
                     symbol.c_str());
        return 1;
    }

    const auto& book = manager.book(*locate);
    const std::vector<book::Level> bids = top(book.bids(), depth);
    const std::vector<book::Level> asks = top(book.asks(), depth);

    std::printf("%s  (locate %u)\n", symbol.c_str(), static_cast<unsigned>(*locate));
    if (at) {
        std::printf("as of           %s\n", util::format_time(*at).c_str());
    } else {
        std::printf("as of           end of file\n");
    }
    if (applied != 0) {
        std::printf("last message    %s\n", util::format_time(last_applied).c_str());
    }
    std::printf("messages        %s applied for this security\n",
                util::with_commas(applied).c_str());
    std::printf("trading state   '%c'\n", book.trading_state());
    std::printf("levels          %zu bid, %zu ask\n\n", book.bids().size(), book.asks().size());

    std::printf("%12s  %12s   |   %-12s  %-12s\n", "bid shares", "bid", "ask", "ask shares");
    const std::size_t rows = bids.size() > asks.size() ? bids.size() : asks.size();
    for (std::size_t i = 0; i < rows; ++i) {
        const std::string bid_qty = i < bids.size() ? util::with_commas(bids[i].qty) : "";
        const std::string bid_px = i < bids.size() ? util::format_price(bids[i].price) : "";
        const std::string ask_px = i < asks.size() ? util::format_price(asks[i].price) : "";
        const std::string ask_qty = i < asks.size() ? util::with_commas(asks[i].qty) : "";
        std::printf("%12s  %12s   |   %-12s  %-12s\n", bid_qty.c_str(), bid_px.c_str(),
                    ask_px.c_str(), ask_qty.c_str());
    }
    if (rows == 0) {
        std::printf("%12s  %12s   |   %-12s  %-12s\n", "-", "-", "-", "-");
    }

    const book::Bbo bbo = book.bbo();
    if (bbo.crossed()) {
        std::printf("\nnote: the book is crossed (best bid above best ask).\n");
    } else if (bbo.locked()) {
        std::printf("\nnote: the book is locked (best bid equals best ask).\n");
    }
    if (!manager.counters().clean()) {
        std::printf("\nwarning: invariant counters are not all zero; run book_replay.\n");
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
