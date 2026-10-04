// itch_stats: parse an ITCH 5.0 file and report what is in it.
//
//   itch_stats <file>            human-readable report
//   itch_stats --counts <file>   one "<type> <count>" line per message type and a
//                                final "total <n>" line, the same output as
//                                scripts/count_messages.py, so the two can be
//                                compared with diff (success criterion 1)
//
// Exit status: 0 ok, 1 usage or I/O error, 2 the file is truncated or corrupt.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

#include "obe/feed/parser.hpp"
#include "obe/util/format.hpp"
#include "obe/util/mapped_file.hpp"

namespace {

using namespace obe;

struct StatsHandler : feed::HandlerBase {
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t total = 0;
    std::uint64_t symbols = 0;
    Nanos first_timestamp = 0;
    Nanos last_timestamp = 0;
    Nanos max_timestamp = 0;
    std::uint64_t timestamp_regressions = 0;

    void note(char type, const feed::Header& hdr) noexcept {
        ++counts[static_cast<unsigned char>(type)];
        if (total == 0) {
            first_timestamp = hdr.timestamp;
        } else if (hdr.timestamp < last_timestamp) {
            ++timestamp_regressions;
        }
        last_timestamp = hdr.timestamp;
        if (hdr.timestamp > max_timestamp) {
            max_timestamp = hdr.timestamp;
        }
        ++total;
    }

    template <class M>
    void note(const M& m) noexcept {
        note(m.type(), m.hdr);
    }

    void on_system_event(const feed::SystemEvent& m) noexcept { note(m); }
    void on_stock_directory(const feed::StockDirectory& m) noexcept {
        note(m);
        ++symbols;
    }
    void on_trading_action(const feed::TradingAction& m) noexcept { note(m); }
    void on_reg_sho(const feed::RegSho& m) noexcept { note(m); }
    void on_market_participant_position(const feed::MarketParticipantPosition& m) noexcept {
        note(m);
    }
    void on_mwcb_decline_level(const feed::MwcbDeclineLevel& m) noexcept { note(m); }
    void on_mwcb_status(const feed::MwcbStatus& m) noexcept { note(m); }
    void on_ipo_quoting_period(const feed::IpoQuotingPeriod& m) noexcept { note(m); }
    void on_luld_auction_collar(const feed::LuldAuctionCollar& m) noexcept { note(m); }
    void on_operational_halt(const feed::OperationalHalt& m) noexcept { note(m); }
    void on_add(const feed::AddOrder& m) noexcept { note(m); }
    void on_execute(const feed::OrderExecuted& m) noexcept { note(m); }
    void on_execute_with_price(const feed::OrderExecutedWithPrice& m) noexcept { note(m); }
    void on_cancel(const feed::OrderCancel& m) noexcept { note(m); }
    void on_delete(const feed::OrderDelete& m) noexcept { note(m); }
    void on_replace(const feed::OrderReplace& m) noexcept { note(m); }
    void on_trade(const feed::Trade& m) noexcept { note(m); }
    void on_cross_trade(const feed::CrossTrade& m) noexcept { note(m); }
    void on_broken_trade(const feed::BrokenTrade& m) noexcept { note(m); }
    void on_noii(const feed::Noii& m) noexcept { note(m); }
    void on_rpii(const feed::Rpii& m) noexcept { note(m); }
    void on_direct_listing(const feed::DirectListingCapitalRaise& m) noexcept { note(m); }
};

constexpr std::string_view message_name(char type) noexcept {
    switch (type) {
        case 'S':
            return "System Event";
        case 'R':
            return "Stock Directory";
        case 'H':
            return "Stock Trading Action";
        case 'Y':
            return "Reg SHO Restriction";
        case 'L':
            return "Market Participant Position";
        case 'V':
            return "MWCB Decline Level";
        case 'W':
            return "MWCB Status";
        case 'K':
            return "IPO Quoting Period Update";
        case 'J':
            return "LULD Auction Collar";
        case 'h':
            return "Operational Halt";
        case 'A':
            return "Add Order";
        case 'F':
            return "Add Order (MPID)";
        case 'E':
            return "Order Executed";
        case 'C':
            return "Order Executed With Price";
        case 'X':
            return "Order Cancel";
        case 'D':
            return "Order Delete";
        case 'U':
            return "Order Replace";
        case 'P':
            return "Trade (non-cross)";
        case 'Q':
            return "Cross Trade";
        case 'B':
            return "Broken Trade";
        case 'I':
            return "NOII";
        case 'N':
            return "Retail Price Improvement";
        case 'O':
            return "Direct Listing Capital Raise";
        default:
            return "?";
    }
}

void print_counts(const StatsHandler& stats) {
    for (std::size_t byte = 0; byte < stats.counts.size(); ++byte) {
        if (stats.counts[byte] != 0) {
            std::printf("%c %llu\n", static_cast<char>(byte),
                        static_cast<unsigned long long>(stats.counts[byte]));
        }
    }
    std::printf("total %llu\n", static_cast<unsigned long long>(stats.total));
}

void print_report(const std::string& path, std::span<const std::byte> buf,
                  const StatsHandler& stats, double seconds) {
    const double msgs_per_s = seconds > 0 ? static_cast<double>(stats.total) / seconds : 0.0;
    const double mb_per_s =
        seconds > 0 ? static_cast<double>(buf.size()) / (1024.0 * 1024.0) / seconds : 0.0;

    std::printf("file:             %s\n", path.c_str());
    std::printf("size:             %s bytes\n", util::with_commas(buf.size()).c_str());
    std::printf("messages:         %s\n", util::with_commas(stats.total).c_str());
    std::printf("symbols:          %s\n", util::with_commas(stats.symbols).c_str());
    if (stats.total != 0) {
        std::printf("first timestamp:  %s\n", util::format_time(stats.first_timestamp).c_str());
        std::printf("last timestamp:   %s\n", util::format_time(stats.last_timestamp).c_str());
    }
    if (stats.timestamp_regressions != 0) {
        std::printf("note:             %s messages have a timestamp earlier than the one before\n",
                    util::with_commas(stats.timestamp_regressions).c_str());
    }
    std::printf("parse time:       %.3f s  (%.2f M msgs/s, %.0f MB/s)\n", seconds, msgs_per_s / 1e6,
                mb_per_s);
    std::printf("                  includes page faults and disk I/O; not a benchmark.\n");
    std::printf("                  Use replay_bench for numbers worth quoting.\n\n");

    std::printf("%-4s  %15s  %7s  %s\n", "type", "count", "share", "message");
    for (const char type : feed::kMessageTypes) {
        const std::uint64_t n = stats.counts[static_cast<unsigned char>(type)];
        const double share = stats.total != 0
                                 ? 100.0 * static_cast<double>(n) / static_cast<double>(stats.total)
                                 : 0.0;
        std::printf("%-4c  %15s  %6.2f%%  %.*s\n", type, util::with_commas(n).c_str(), share,
                    static_cast<int>(message_name(type).size()), message_name(type).data());
    }
}

int run(int argc, char** argv) {
    bool counts_only = false;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--counts") {
            counts_only = true;
        } else if (arg == "-h" || arg == "--help") {
            std::printf("usage: itch_stats [--counts] <uncompressed ITCH 5.0 file>\n");
            return 0;
        } else if (path.empty()) {
            path = arg;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]);
            return 1;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: itch_stats [--counts] <uncompressed ITCH 5.0 file>\n");
        return 1;
    }

    const util::MappedFile file(path);
    const std::span<const std::byte> buf = file.bytes();
    if (util::looks_gzipped(buf)) {
        std::fprintf(stderr,
                     "error: '%s' is gzip-compressed. Decompress it first (gzip -dk), or fetch "
                     "with scripts/fetch_data.sh -x.\n",
                     path.c_str());
        return 1;
    }

    // Spec section 4.2: the first record of a full-day file should be a
    // 12-byte System Event. If it is not, the framing assumption is wrong and
    // nothing after it can be trusted.
    if (!counts_only && buf.size() >= 3) {
        const std::size_t first_len = feed::load_be<std::uint16_t>(buf.data());
        const char first_type = static_cast<char>(buf[2]);
        if (first_len != 12 || first_type != 'S') {
            std::fprintf(stderr,
                         "warning: first record has length %zu and type 0x%02x; a full-day file "
                         "starts with a 12-byte 'S'. Check the framing (or this is a slice).\n",
                         first_len, static_cast<unsigned>(static_cast<unsigned char>(first_type)));
        }
    }

    StatsHandler stats;
    feed::ItchParser parser(stats);
    const auto start = std::chrono::steady_clock::now();
    const feed::ParseResult result = parser.parse(buf);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    if (counts_only) {
        print_counts(stats);
    } else {
        print_report(path, buf, stats, elapsed.count());
    }

    if (!result.ok()) {
        const std::string_view why = feed::to_string(result.status);
        std::fprintf(stderr, "error: %.*s at byte offset %zu, after %llu good messages\n",
                     static_cast<int>(why.size()), why.data(), result.offset,
                     static_cast<unsigned long long>(result.messages));
        return 2;
    }
    return 0;
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
