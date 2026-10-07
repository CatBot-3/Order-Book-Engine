// tick_store: keep a day's best bids and offers on disk, and ask it questions.
//
//   tick_store record [--codec NAME] [--block N] <ITCH file> <store>
//       Replay the file through the book and store every best-bid-and-offer
//       update it publishes.
//   tick_store info [--blocks] <store>
//       What the store holds, and with --blocks the index, block by block.
//   tick_store query [--from T] [--to T] [--symbol S | --locate N]
//                    [--limit N] [--count] [--nanos] <store>
//       The ticks with from <= time < to, as CSV. T is HH:MM:SS, optionally
//       with a fraction, or a number of nanoseconds since midnight.
//   tick_store verify [--against <ITCH file>] <store>
//       Read every block. With --against, also replay the feed and check that
//       the store holds exactly the updates the book publishes.
//   tick_store profile <ITCH file or store>
//       What consecutive ticks differ by: the table to design a codec from.
//   tick_store --list
//       The codec names.
//
// The format is described in include/obe/store/tick_file.hpp. A store that
// was never finished (its recorder was killed) is read up to its last whole
// block: `info` says so.
//
// Exit status: 0 success, 1 usage or I/O error, 2 the store is damaged or is
// not a store, or the ITCH file is corrupt, 3 the store does not match the
// feed, 4 the chosen codec is not written yet.

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "obe/book/bbo_hash.hpp"
#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/feed/messages.hpp"
#include "obe/feed/parser.hpp"
#include "obe/store/codecs.hpp"
#include "obe/store/tick_codec.hpp"
#include "obe/store/tick_file.hpp"
#include "obe/store/tick_profile.hpp"
#include "obe/types.hpp"
#include "obe/util/format.hpp"
#include "obe/util/mapped_file.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;
using store::Tick;

struct Options {
    std::string command;
    std::vector<std::string> paths;
    std::string codec = "raw";
    std::uint32_t block = store::kDefaultTicksPerBlock;
    std::string against;
    Nanos from = 0;
    Nanos to = store::kEndOfTime;
    std::string symbol;
    std::optional<Locate> locate;
    std::uint64_t limit = 0;  // 0: no limit
    bool count_only = false;
    bool nanos = false;
    bool show_blocks = false;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: tick_store record [--codec NAME] [--block N] <ITCH file> <store>\n"
                 "       tick_store info [--blocks] <store>\n"
                 "       tick_store query [--from T] [--to T] [--symbol S | --locate N]\n"
                 "                        [--limit N] [--count] [--nanos] <store>\n"
                 "       tick_store verify [--against <ITCH file>] <store>\n"
                 "       tick_store profile <ITCH file or store>\n"
                 "       tick_store --list\n"
                 "T is HH:MM:SS[.fraction] or nanoseconds since midnight.\n");
    return status;
}

void line(const char* label, const std::string& value) {
    std::printf("  %-24s %s\n", label, value.c_str());
}
void line(const char* label, std::uint64_t value) {
    line(label, util::with_commas(value));
}

std::string text(std::string_view s) {
    return std::string(s);
}

std::string decimal(double v, int places) {
    std::array<char, 48> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%.*f", places, v);
    return {buf.data(), static_cast<std::size_t>(n)};
}

double ratio(std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? 0.0 : static_cast<double>(part) / static_cast<double>(whole);
}

template <class T>
bool parse_number(std::string_view s, T& out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

std::optional<Nanos> parse_when(std::string_view s) {
    if (const std::optional<Nanos> t = util::parse_time(s)) {
        return t;
    }
    Nanos n = 0;
    if (parse_number(s, n)) {
        return n;
    }
    return std::nullopt;
}

util::MappedFile map_feed(const std::string& path) {
    util::MappedFile file(path);
    if (util::looks_gzipped(file.bytes())) {
        throw std::runtime_error("'" + path + "' is gzip-compressed. Decompress it first.");
    }
    return file;
}

int report_feed_error(const feed::ParseResult& result) {
    const std::string_view why = feed::to_string(result.status);
    std::fprintf(stderr, "error: %.*s at byte offset %zu, after %" PRIu64 " good messages\n",
                 static_cast<int>(why.size()), why.data(), result.offset, result.messages);
    return 2;
}

// --- record --------------------------------------------------------------------------

// The destination of a store being recorded: a file, through stdio's buffer.
// A tick store is not a journal. Nothing waits on it reaching the disk, and a
// store that lost its last blocks is still a store.
class ToStdio {
 public:
    explicit ToStdio(std::FILE* file) noexcept : file_(file) {}
    void operator()(std::span<const std::byte> bytes) const {
        if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
            throw std::runtime_error(std::string("cannot write the store: ") +
                                     std::strerror(errno));
        }
    }

 private:
    std::FILE* file_;
};

struct CloseFile {
    void operator()(std::FILE* f) const noexcept {
        if (f != nullptr) {
            std::fclose(f);
        }
    }
};

template <class Codec>
int record(const Options& opt) {
    const util::MappedFile in = map_feed(opt.paths[0]);
    {
        // A codec that is not written yet says so here, before a file with
        // nothing but a header is left behind under the store's name.
        Codec probe;
        probe.reset();
    }
    std::unique_ptr<std::FILE, CloseFile> out(std::fopen(opt.paths[1].c_str(), "wb"));
    if (!out) {
        throw std::runtime_error("cannot create '" + opt.paths[1] + "': " + std::strerror(errno));
    }

    using Recorder = store::TickRecorder<Codec, ToStdio>;
    using Books = book::BookManager<book::OrderStore, book::PriceLevels, Recorder>;
    store::TickWriter<Codec, ToStdio> writer(ToStdio(out.get()), opt.block);
    const auto books = std::make_unique<Books>(book::OrderStore{}, Recorder(writer));
    feed::ItchParser parser(*books);

    const auto start = std::chrono::steady_clock::now();
    const feed::ParseResult result = parser.parse(in.bytes());
    for (std::size_t i = 0; i < Books::kLocates; ++i) {
        const auto locate = static_cast<Locate>(i);
        const std::string_view symbol = books->symbol(locate);
        if (!symbol.empty()) {
            writer.add_symbol(locate, feed::Symbol::from(symbol));
        }
    }
    writer.finish();
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    if (std::fclose(out.release()) != 0) {
        throw std::runtime_error("cannot write '" + opt.paths[1] + "': " + std::strerror(errno));
    }

    const std::uint64_t raw = writer.ticks() * store::RawCodec::kMaxTickSize;
    std::printf("recorded (%.*s: %.*s)\n", static_cast<int>(Codec::kName.size()),
                Codec::kName.data(), static_cast<int>(Codec::kDescription.size()),
                Codec::kDescription.data());
    line("messages read", result.messages);
    line("ticks", writer.ticks());
    line("blocks", util::with_commas(writer.blocks()) + "  (up to " +
                       util::with_commas(writer.ticks_per_block()) + " ticks each)");
    line("file, bytes", writer.bytes());
    line("of which ticks", writer.payload_bytes());
    line("bytes per tick",
         decimal(ratio(writer.payload_bytes(), writer.ticks()), 2) +
             "  (at full width: " + std::to_string(store::RawCodec::kMaxTickSize) + ")");
    line("against full width", decimal(ratio(raw, writer.payload_bytes()), 2) + " times smaller");
    line("seconds", decimal(took.count(), 3) + "  (reading the feed, the books and the store)");
    return result.ok() ? 0 : report_feed_error(result);
}

// --- info ----------------------------------------------------------------------------

struct Range {
    Nanos earliest = 0;
    Nanos latest = 0;
    std::uint64_t payload = 0;
};

Range range_of(const std::vector<store::BlockInfo>& blocks) {
    Range r;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        r.earliest = i == 0 ? blocks[i].earliest : std::min(r.earliest, blocks[i].earliest);
        r.latest = std::max(r.latest, blocks[i].latest);
        r.payload += blocks[i].payload;
    }
    return r;
}

template <class Codec>
int info(const Options& opt, std::span<const std::byte> file) {
    const store::TickReader<Codec> reader(file);
    const Range range = range_of(reader.blocks());
    std::printf("%s\n", opt.paths[0].c_str());
    line("codec", text(Codec::kName) + "  (" + text(Codec::kDescription) + ")");
    line("state", text(store::to_string(reader.status())));
    line("ticks per block, up to", reader.ticks_per_block());
    line("blocks", reader.blocks().size());
    line("ticks", reader.ticks());
    line("file, bytes", file.size());
    if (reader.good_bytes() != file.size()) {
        line("of which accounted for", reader.good_bytes());
    }
    line("bytes per tick", decimal(ratio(range.payload, reader.ticks()), 2));
    if (!reader.blocks().empty()) {
        line("earliest tick", util::format_time(range.earliest));
        line("latest tick", util::format_time(range.latest));
    }
    line("securities named", reader.symbols().size());
    if (opt.show_blocks) {
        std::printf("\n%8s %14s %8s %10s  %-18s  %-18s\n", "block", "offset", "ticks", "bytes",
                    "earliest", "latest");
        for (std::size_t i = 0; i < reader.blocks().size(); ++i) {
            const store::BlockInfo& b = reader.blocks()[i];
            std::printf("%8zu %14" PRIu64 " %8u %10u  %-18s  %-18s\n", i, b.offset, b.count,
                        b.payload, util::format_time(b.earliest).c_str(),
                        util::format_time(b.latest).c_str());
        }
    }
    return 0;
}

// --- query ---------------------------------------------------------------------------

int report_damage(const store::ScanResult& result, std::size_t blocks) {
    std::fprintf(stderr,
                 "error: block %zu of %zu does not match its checksum or does not decode. "
                 "The %" PRIu64 " ticks before it were read.\n",
                 result.bad_block, blocks, result.ticks);
    return 2;
}

template <class Codec>
int query(const Options& opt, std::span<const std::byte> file) {
    const store::TickReader<Codec> reader(file);
    std::optional<Locate> only = opt.locate;
    if (!opt.symbol.empty()) {
        only = reader.locate_of(opt.symbol);
        if (!only) {
            if (reader.status() == store::OpenStatus::Recovered) {
                std::fprintf(stderr,
                             "error: this store has no directory of names (it was never "
                             "finished). Use --locate.\n");
            } else {
                std::fprintf(stderr, "error: the store names no security '%s'\n",
                             opt.symbol.c_str());
            }
            return 1;
        }
    }
    std::vector<std::string> names(std::size_t{1} << 16);
    for (const store::SymbolEntry& entry : reader.symbols()) {
        names[entry.locate] = text(entry.symbol.view());
    }

    if (!opt.count_only) {
        std::printf("time,symbol,locate,bid_price,bid_shares,ask_price,ask_shares\n");
    }
    std::uint64_t matched = 0;
    const store::ScanResult result = reader.scan(opt.from, opt.to, [&](const Tick& tick) {
        if (only && tick.locate != *only) {
            return true;
        }
        ++matched;
        if (!opt.count_only) {
            const std::string when =
                opt.nanos ? std::to_string(tick.timestamp) : util::format_time(tick.timestamp);
            std::printf("%s,%s,%u,%s,%" PRIu64 ",%s,%" PRIu64 "\n", when.c_str(),
                        names[tick.locate].c_str(), static_cast<unsigned>(tick.locate),
                        util::format_price(tick.bbo.bid_price).c_str(), tick.bbo.bid_qty,
                        util::format_price(tick.bbo.ask_price).c_str(), tick.bbo.ask_qty);
        }
        return opt.limit == 0 || matched < opt.limit;
    });
    if (opt.count_only) {
        std::printf("%" PRIu64 "\n", matched);
    }
    std::fprintf(stderr,
                 "%" PRIu64 " ticks; %" PRIu64 " of %zu blocks read, %" PRIu64
                 " ruled out by their time range\n",
                 matched, result.blocks_read, reader.blocks().size(), result.blocks_skipped);
    return result.ok ? 0 : report_damage(result, reader.blocks().size());
}

// --- verify --------------------------------------------------------------------------

template <class Codec>
int verify(const Options& opt, std::span<const std::byte> file) {
    const store::TickReader<Codec> reader(file);
    // One running hash per security over every field of every tick, in
    // order: the instrument that compares two books (book/bbo_hash.hpp).
    const auto stored = std::make_unique<book::BboHasher>();
    const store::ScanResult result = reader.scan([&stored](const Tick& tick) {
        stored->on_bbo(tick);
        return true;
    });
    if (!result.ok) {
        return report_damage(result, reader.blocks().size());
    }
    std::printf("%s ticks in %s blocks: every block matches its checksum and decodes\n",
                util::with_commas(result.ticks).c_str(),
                util::with_commas(reader.blocks().size()).c_str());
    if (reader.status() == store::OpenStatus::Recovered) {
        std::printf(
            "note: the store has no index. It was read up to its last whole block, "
            "byte %s of %s.\n",
            util::with_commas(reader.good_bytes()).c_str(), util::with_commas(file.size()).c_str());
    }
    if (opt.against.empty()) {
        return 0;
    }

    const util::MappedFile feed_file = map_feed(opt.against);
    using Books = book::BookManager<book::OrderStore, book::PriceLevels, book::BboHasher>;
    const auto books = std::make_unique<Books>();
    feed::ItchParser parser(*books);
    const feed::ParseResult parsed = parser.parse(feed_file.bytes());
    if (!parsed.ok()) {
        return report_feed_error(parsed);
    }
    const book::BboHasher& published = books->listener();
    std::printf("the book publishes   %s updates, hash %016" PRIx64 "\n",
                util::with_commas(published.total_events()).c_str(), published.combined());
    std::printf("the store holds      %s ticks,   hash %016" PRIx64 "\n",
                util::with_commas(stored->total_events()).c_str(), stored->combined());
    if (published.total_events() != stored->total_events() ||
        published.combined() != stored->combined()) {
        std::printf("RESULT: the store does not match the feed\n");
        return 3;
    }
    std::printf("RESULT: the store holds exactly what the book publishes\n");
    return 0;
}

// --- profile -------------------------------------------------------------------------

constexpr std::array<const char*, store::Magnitudes::kClasses> kClassNames = {
    "0", "under 128", "under 16,384", "under 2,097,152", "under 268,435,456", "larger",
};

std::string percent(std::uint64_t part, std::uint64_t whole) {
    return decimal(100.0 * ratio(part, whole), 1) + "%";
}

void print_magnitudes(const store::Magnitudes& m) {
    for (std::size_t k = 0; k < store::Magnitudes::kClasses; ++k) {
        std::printf("    %-20s %7s  %s\n", kClassNames[k], percent(m.count[k], m.total()).c_str(),
                    util::with_commas(m.count[k]).c_str());
    }
}

std::string mask_name(unsigned mask) {
    if (mask == 0) {
        return "nothing";
    }
    static constexpr std::array<const char*, 4> kFields = {"bid price", "bid size", "ask price",
                                                           "ask size"};
    std::string out;
    for (unsigned bit = 0; bit < 4; ++bit) {
        if ((mask & (1U << bit)) != 0) {
            out += out.empty() ? "" : " + ";
            out += kFields[bit];
        }
    }
    return out;
}

void print_profile(const store::TickProfile& p) {
    std::printf("%s ticks over %s securities\n", util::with_commas(p.ticks()).c_str(),
                util::with_commas(p.securities()).c_str());
    if (p.ticks() < 2) {
        return;
    }
    std::printf("\ntime since the tick before, of any security (nanoseconds)\n");
    print_magnitudes(p.time_gap());
    std::printf("    %-20s %7s  %s\n", "of which backwards",
                percent(p.time_backwards(), p.time_gap().total()).c_str(),
                util::with_commas(p.time_backwards()).c_str());

    std::printf("\nthe security\n");
    std::printf("    %-20s %7s\n", "same as tick before",
                percent(p.same_security(), p.ticks() - 1).c_str());
    std::printf("  ticks since its own last tick (1 is the tick before)\n");
    print_magnitudes(p.ticks_since());

    const std::uint64_t compared = p.ticks_since().total();
    std::printf("\nwhich fields differ from the security's last tick\n");
    std::array<unsigned, 16> order{};
    for (unsigned i = 0; i < 16; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&p](unsigned a, unsigned b) { return p.masks()[a] > p.masks()[b]; });
    for (const unsigned mask : order) {
        if (p.masks()[mask] == 0) {
            break;
        }
        std::printf("    %-44s %7s\n", mask_name(mask).c_str(),
                    percent(p.masks()[mask], compared).c_str());
    }

    const std::array<const store::FieldChanges*, 4> fields = {&p.bid_price(), &p.bid_qty(),
                                                              &p.ask_price(), &p.ask_qty()};
    std::printf("\nhow a field changed, on the ticks where it did\n");
    std::printf("    %-20s %10s %10s %10s %10s\n", "", "bid price", "bid size", "ask price",
                "ask size");
    const auto row = [&fields](const char* label, auto&& part) {
        std::printf("    %-20s", label);
        for (const store::FieldChanges* f : fields) {
            std::printf(" %10s", percent(part(*f), f->changes).c_str());
        }
        std::printf("\n");
    };
    std::printf("    %-20s", "changed at all");
    for (const store::FieldChanges* f : fields) {
        std::printf(" %10s", percent(f->changes, compared).c_str());
    }
    std::printf("\n");
    row("went down", [](const store::FieldChanges& f) { return f.down; });
    row("to or from zero", [](const store::FieldChanges& f) { return f.to_or_from_zero; });
    row("a multiple of 100", [](const store::FieldChanges& f) { return f.hundreds; });
    row("exactly 100", [](const store::FieldChanges& f) { return f.one_hundred; });
    std::printf("  by how much, either way\n");
    for (std::size_t k = 1; k < store::Magnitudes::kClasses; ++k) {
        row(kClassNames[k], [k](const store::FieldChanges& f) { return f.size.count[k]; });
    }
    std::printf("\nPrices are in 1/10000 of a dollar, so 100 is one cent. Sizes are shares.\n");
}

template <class Codec>
int profile_store(std::span<const std::byte> file) {
    const store::TickReader<Codec> reader(file);
    const auto profile = std::make_unique<store::TickProfile>();
    const store::ScanResult result = reader.scan([&profile](const Tick& tick) {
        profile->add(tick);
        return true;
    });
    print_profile(*profile);
    return result.ok ? 0 : report_damage(result, reader.blocks().size());
}

int profile_feed(std::span<const std::byte> feed_bytes) {
    using Books = book::BookManager<book::OrderStore, book::PriceLevels, store::TickProfile>;
    const auto books = std::make_unique<Books>();
    feed::ItchParser parser(*books);
    const feed::ParseResult result = parser.parse(feed_bytes);
    print_profile(books->listener());
    return result.ok() ? 0 : report_feed_error(result);
}

// --- main ----------------------------------------------------------------------------

// Opens a store with the codec its header names and calls f<Codec>(bytes).
template <class F>
int with_store(const std::string& path, F&& f) {
    const util::MappedFile file(path, util::MappedFile::Advice::None);
    const std::optional<store::FileHeader> header = store::read_header(file.bytes());
    if (!header) {
        std::fprintf(stderr, "error: '%s' is not a tick store this version can read\n",
                     path.c_str());
        return 2;
    }
    int status = 2;
    const bool known =
        store::with_codec_id(header->codec, [&]<class Codec>(std::type_identity<Codec>) {
            status = f.template operator()<Codec>(file.bytes());
        });
    if (!known) {
        std::fprintf(stderr, "error: '%s' was written with codec %u, which this build lacks\n",
                     path.c_str(), static_cast<unsigned>(header->codec));
    }
    return status;
}

int run(int argc, char** argv) {
    if (argc < 2) {
        return usage(stderr, 1);
    }
    Options opt;
    opt.command = argv[1];
    if (opt.command == "-h" || opt.command == "--help") {
        return usage(stdout, 0);
    }
    if (opt.command == "--list") {
        store::for_each_codec([]<class Codec>(std::type_identity<Codec>) {
            std::printf("%-8.*s %.*s\n", static_cast<int>(Codec::kName.size()), Codec::kName.data(),
                        static_cast<int>(Codec::kDescription.size()), Codec::kDescription.data());
        });
        return 0;
    }
    const auto bad = [](const char* what) {
        std::fprintf(stderr, "error: %s\n", what);
        return 1;
    };
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = i + 1 < argc;
        if (arg == "--codec" && has_next) {
            opt.codec = argv[++i];
        } else if (arg == "--block" && has_next) {
            if (!parse_number(argv[++i], opt.block) || opt.block == 0 ||
                opt.block > store::kMaxTicksPerBlock) {
                return bad("--block needs a number of ticks from 1 to 65536");
            }
        } else if (arg == "--against" && has_next) {
            opt.against = argv[++i];
        } else if ((arg == "--from" || arg == "--to") && has_next) {
            const std::optional<Nanos> when = parse_when(argv[++i]);
            if (!when) {
                return bad("--from and --to need HH:MM:SS[.fraction] or nanoseconds");
            }
            (arg == "--from" ? opt.from : opt.to) = *when;
        } else if (arg == "--symbol" && has_next) {
            opt.symbol = argv[++i];
        } else if (arg == "--locate" && has_next) {
            Locate locate = 0;
            if (!parse_number(argv[++i], locate)) {
                return bad("--locate needs a number from 0 to 65535");
            }
            opt.locate = locate;
        } else if (arg == "--limit" && has_next) {
            if (!parse_number(argv[++i], opt.limit)) {
                return bad("--limit needs a non-negative integer");
            }
        } else if (arg == "--count") {
            opt.count_only = true;
        } else if (arg == "--nanos") {
            opt.nanos = true;
        } else if (arg == "--blocks") {
            opt.show_blocks = true;
        } else if (!arg.starts_with("-")) {
            opt.paths.emplace_back(arg);
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (!opt.symbol.empty() && opt.locate) {
        return bad("give --symbol or --locate, not both");
    }

    if (opt.command == "record") {
        if (opt.paths.size() != 2) {
            return usage(stderr, 1);
        }
        int status = 1;
        const bool known = store::with_codec(
            opt.codec,
            [&]<class Codec>(std::type_identity<Codec>) { status = record<Codec>(opt); });
        if (!known) {
            std::fprintf(stderr, "error: no codec called '%s'. Try --list.\n", opt.codec.c_str());
        }
        return status;
    }
    if (opt.paths.size() != 1) {
        return usage(stderr, 1);
    }
    if (opt.command == "info") {
        return with_store(opt.paths[0], [&opt]<class Codec>(std::span<const std::byte> file) {
            return info<Codec>(opt, file);
        });
    }
    if (opt.command == "query") {
        return with_store(opt.paths[0], [&opt]<class Codec>(std::span<const std::byte> file) {
            return query<Codec>(opt, file);
        });
    }
    if (opt.command == "verify") {
        return with_store(opt.paths[0], [&opt]<class Codec>(std::span<const std::byte> file) {
            return verify<Codec>(opt, file);
        });
    }
    if (opt.command == "profile") {
        const util::MappedFile file = map_feed(opt.paths[0]);
        if (!store::read_header(file.bytes())) {
            return profile_feed(file.bytes());
        }
        return with_store(opt.paths[0], []<class Codec>(std::span<const std::byte> bytes) {
            return profile_store<Codec>(bytes);
        });
    }
    std::fprintf(stderr, "error: no command called '%s'\n", opt.command.c_str());
    return usage(stderr, 1);
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
