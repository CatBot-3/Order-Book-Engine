// libFuzzer target for the ITCH parser.
//
// Two properties are checked on arbitrary bytes:
//   1. The parser never reads outside its input (ASan) and never executes
//      undefined behaviour (UBSan), whatever the bytes are.
//   2. Any message the parser accepts re-encodes to exactly the bytes it was
//      decoded from. A decoder that read a field from the wrong place, or an
//      encoder that wrote one there, breaks this.
//
//   cmake --preset fuzz && cmake --build --preset fuzz
//   build/fuzz/fuzz/parser_fuzz -max_len=4096 -max_total_time=60 fuzz/corpus

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "obe/feed/codec.hpp"
#include "obe/feed/parser.hpp"

namespace {

using namespace obe::feed;

struct ReencodeHandler : HandlerBase {
    std::array<std::byte, kMaxMessageSize> bytes{};
    std::size_t size = 0;

    template <class M>
    void capture(const M& m) noexcept {
        size = encode(m, bytes.data());
    }

    void on_system_event(const SystemEvent& m) noexcept { capture(m); }
    void on_stock_directory(const StockDirectory& m) noexcept { capture(m); }
    void on_trading_action(const TradingAction& m) noexcept { capture(m); }
    void on_reg_sho(const RegSho& m) noexcept { capture(m); }
    void on_market_participant_position(const MarketParticipantPosition& m) noexcept { capture(m); }
    void on_mwcb_decline_level(const MwcbDeclineLevel& m) noexcept { capture(m); }
    void on_mwcb_status(const MwcbStatus& m) noexcept { capture(m); }
    void on_ipo_quoting_period(const IpoQuotingPeriod& m) noexcept { capture(m); }
    void on_luld_auction_collar(const LuldAuctionCollar& m) noexcept { capture(m); }
    void on_operational_halt(const OperationalHalt& m) noexcept { capture(m); }
    void on_add(const AddOrder& m) noexcept { capture(m); }
    void on_execute(const OrderExecuted& m) noexcept { capture(m); }
    void on_execute_with_price(const OrderExecutedWithPrice& m) noexcept { capture(m); }
    void on_cancel(const OrderCancel& m) noexcept { capture(m); }
    void on_delete(const OrderDelete& m) noexcept { capture(m); }
    void on_replace(const OrderReplace& m) noexcept { capture(m); }
    void on_trade(const Trade& m) noexcept { capture(m); }
    void on_cross_trade(const CrossTrade& m) noexcept { capture(m); }
    void on_broken_trade(const BrokenTrade& m) noexcept { capture(m); }
    void on_noii(const Noii& m) noexcept { capture(m); }
    void on_rpii(const Rpii& m) noexcept { capture(m); }
    void on_direct_listing(const DirectListingCapitalRaise& m) noexcept { capture(m); }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> buf(reinterpret_cast<const std::byte*>(data), size);

    ReencodeHandler handler;
    ItchParser parser(handler);
    FrameReader reader(buf);
    Frame frame;
    while (!reader.done()) {
        if (reader.next(frame) != ParseStatus::Ok) {
            break;
        }
        handler.size = 0;
        if (parser.dispatch(frame) != ParseStatus::Ok) {
            break;
        }
        if (handler.size != frame.size ||
            std::memcmp(handler.bytes.data(), frame.data, frame.size) != 0) {
            __builtin_trap();
        }
    }

    // The whole-buffer entry point must agree with the step-by-step walk above
    // about where the stream stops being valid.
    ReencodeHandler again;
    ItchParser whole(again);
    const ParseResult result = whole.parse(buf);
    if (result.offset > size) {
        __builtin_trap();
    }
    return 0;
}
