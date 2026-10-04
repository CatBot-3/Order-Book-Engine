#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "obe/types.hpp"

// Decoded forms of every Nasdaq TotalView-ITCH 5.0 message.
//
// Each struct lists its wire fields once, in wire order, in visit(). The
// decoder, the encoder and the size check in codec.hpp are all generated from
// that one list, so they cannot drift apart. The sizes are checked against the
// specification at compile time (see the static_asserts in codec.hpp), and the
// byte offsets are pinned by hand-built byte arrays in tests/feed.
//
// Field names follow the specification. Character codes are kept as raw bytes
// rather than enums: the feed handler does not act on most of them, and keeping
// the byte means a decoded message re-encodes to exactly the bytes it came from.

namespace obe::feed {

// Common to every message: bytes 1 to 10. Byte 0 is the message type.
struct Header {
    Locate locate = 0;
    std::uint16_t tracking = 0;
    Nanos timestamp = 0;  // nanoseconds since midnight, 48 bits on the wire

    friend bool operator==(const Header&, const Header&) = default;
};

// Eight ASCII characters, left-justified and space-padded.
struct Symbol {
    std::array<char, 8> raw{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

    [[nodiscard]] static constexpr Symbol from(std::string_view s) noexcept {
        Symbol out;
        for (std::size_t i = 0; i < out.raw.size() && i < s.size(); ++i) {
            out.raw[i] = s[i];
        }
        return out;
    }

    // The symbol without its padding. Valid for as long as this Symbol lives.
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        std::size_t n = raw.size();
        while (n > 0 && raw[n - 1] == ' ') {
            --n;
        }
        return {raw.data(), n};
    }

    friend bool operator==(const Symbol&, const Symbol&) = default;
};

namespace detail {
template <class H, class V>
constexpr void visit_header(H& h, V& v) {
    v.u16(h.locate);
    v.u16(h.tracking);
    v.u48(h.timestamp);
}
}  // namespace detail

// ---------------------------------------------------------------------------
// Administrative messages. None of these changes the displayed book.
// ---------------------------------------------------------------------------

// 'S' System Event. Marks the phases of the day: 'O' start of messages,
// 'S' start of system hours, 'Q' start of market hours, 'M' end of market
// hours, 'E' end of system hours, 'C' end of messages.
struct SystemEvent {
    static constexpr char kType = 'S';
    Header hdr{};
    char event_code = ' ';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.ch(m.event_code);
    }
    friend bool operator==(const SystemEvent&, const SystemEvent&) = default;
};

// 'R' Stock Directory. One per security at the start of the day. This is the
// only place the locate-to-symbol mapping comes from.
struct StockDirectory {
    static constexpr char kType = 'R';
    Header hdr{};
    Symbol stock{};
    char market_category = ' ';
    char financial_status = ' ';
    std::uint32_t round_lot_size = 0;
    char round_lots_only = ' ';
    char issue_classification = ' ';
    std::array<char, 2> issue_subtype{' ', ' '};
    char authenticity = ' ';
    char short_sale_threshold = ' ';
    char ipo_flag = ' ';
    char luld_tier = ' ';
    char etp_flag = ' ';
    std::uint32_t etp_leverage = 0;
    char inverse_indicator = ' ';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.chars(m.stock.raw);
        v.ch(m.market_category);
        v.ch(m.financial_status);
        v.u32(m.round_lot_size);
        v.ch(m.round_lots_only);
        v.ch(m.issue_classification);
        v.chars(m.issue_subtype);
        v.ch(m.authenticity);
        v.ch(m.short_sale_threshold);
        v.ch(m.ipo_flag);
        v.ch(m.luld_tier);
        v.ch(m.etp_flag);
        v.u32(m.etp_leverage);
        v.ch(m.inverse_indicator);
    }
    friend bool operator==(const StockDirectory&, const StockDirectory&) = default;
};

// 'H' Stock Trading Action. Halt and resume state: 'H' halted, 'P' paused,
// 'Q' quotation only, 'T' trading.
struct TradingAction {
    static constexpr char kType = 'H';
    Header hdr{};
    Symbol stock{};
    char trading_state = ' ';
    char reserved = ' ';
    std::array<char, 4> reason{' ', ' ', ' ', ' '};

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.chars(m.stock.raw);
        v.ch(m.trading_state);
        v.ch(m.reserved);
        v.chars(m.reason);
    }
    friend bool operator==(const TradingAction&, const TradingAction&) = default;
};

// 'Y' Reg SHO Short Sale Price Test Restricted Indicator.
struct RegSho {
    static constexpr char kType = 'Y';
    Header hdr{};
    Symbol stock{};
    char action = ' ';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.chars(m.stock.raw);
        v.ch(m.action);
    }
    friend bool operator==(const RegSho&, const RegSho&) = default;
};

// 'L' Market Participant Position.
struct MarketParticipantPosition {
    static constexpr char kType = 'L';
    Header hdr{};
    std::array<char, 4> mpid{' ', ' ', ' ', ' '};
    Symbol stock{};
    char primary_market_maker = ' ';
    char market_maker_mode = ' ';
    char participant_state = ' ';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.chars(m.mpid);
        v.chars(m.stock.raw);
        v.ch(m.primary_market_maker);
        v.ch(m.market_maker_mode);
        v.ch(m.participant_state);
    }
    friend bool operator==(const MarketParticipantPosition&,
                           const MarketParticipantPosition&) = default;
};

// 'V' Market-Wide Circuit Breaker decline levels. Eight implied decimals.
struct MwcbDeclineLevel {
    static constexpr char kType = 'V';
    Header hdr{};
    Price8 level1 = 0;
    Price8 level2 = 0;
    Price8 level3 = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.level1);
        v.u64(m.level2);
        v.u64(m.level3);
    }
    friend bool operator==(const MwcbDeclineLevel&, const MwcbDeclineLevel&) = default;
};

// 'W' Market-Wide Circuit Breaker status.
struct MwcbStatus {
    static constexpr char kType = 'W';
    Header hdr{};
    char breached_level = ' ';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.ch(m.breached_level);
    }
    friend bool operator==(const MwcbStatus&, const MwcbStatus&) = default;
};

// 'K' IPO Quoting Period Update.
struct IpoQuotingPeriod {
    static constexpr char kType = 'K';
    Header hdr{};
    Symbol stock{};
    std::uint32_t release_time = 0;  // seconds since midnight
    char release_qualifier = ' ';
    Price ipo_price = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.chars(m.stock.raw);
        v.u32(m.release_time);
        v.ch(m.release_qualifier);
        v.u32(m.ipo_price);
    }
    friend bool operator==(const IpoQuotingPeriod&, const IpoQuotingPeriod&) = default;
};

// 'J' Limit Up-Limit Down Auction Collar.
struct LuldAuctionCollar {
    static constexpr char kType = 'J';
    Header hdr{};
    Symbol stock{};
    Price reference_price = 0;
    Price upper_price = 0;
    Price lower_price = 0;
    std::uint32_t extension = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.chars(m.stock.raw);
        v.u32(m.reference_price);
        v.u32(m.upper_price);
        v.u32(m.lower_price);
        v.u32(m.extension);
    }
    friend bool operator==(const LuldAuctionCollar&, const LuldAuctionCollar&) = default;
};

// 'h' Operational Halt.
struct OperationalHalt {
    static constexpr char kType = 'h';
    Header hdr{};
    Symbol stock{};
    char market_code = ' ';
    char action = ' ';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.chars(m.stock.raw);
        v.ch(m.market_code);
        v.ch(m.action);
    }
    friend bool operator==(const OperationalHalt&, const OperationalHalt&) = default;
};

// ---------------------------------------------------------------------------
// Order messages. These seven are the only ones that change the displayed book.
// ---------------------------------------------------------------------------

// 'A' Add Order and 'F' Add Order with MPID attribution. The two differ only in
// the trailing four-byte attribution, so they share one struct. `attributed`
// records which one was on the wire and decides which one is written back.
struct AddOrder {
    static constexpr char kType = 'A';
    static constexpr char kTypeAttributed = 'F';
    Header hdr{};
    OrderId order_ref = 0;
    Side side = Side::Buy;
    Qty shares = 0;
    Symbol stock{};
    Price price = 0;
    std::array<char, 4> mpid{' ', ' ', ' ', ' '};
    bool attributed = false;

    [[nodiscard]] constexpr char type() const noexcept {
        return attributed ? kTypeAttributed : kType;
    }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.order_ref);
        v.side(m.side);
        v.u32(m.shares);
        v.chars(m.stock.raw);
        v.u32(m.price);
        if (m.attributed) {
            v.chars(m.mpid);
        }
    }
    friend bool operator==(const AddOrder&, const AddOrder&) = default;
};

// 'E' Order Executed. Note what is missing: no side, no price, no symbol. The
// receiver has to look the order up by reference number to learn them.
struct OrderExecuted {
    static constexpr char kType = 'E';
    Header hdr{};
    OrderId order_ref = 0;
    Qty shares = 0;
    std::uint64_t match_number = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.order_ref);
        v.u32(m.shares);
        v.u64(m.match_number);
    }
    friend bool operator==(const OrderExecuted&, const OrderExecuted&) = default;
};

// 'C' Order Executed With Price. The execution price can differ from the
// order's display price. `printable` is 'Y' or 'N'; non-printable executions
// are left out of volume because a later cross message reports them in bulk.
struct OrderExecutedWithPrice {
    static constexpr char kType = 'C';
    Header hdr{};
    OrderId order_ref = 0;
    Qty shares = 0;
    std::uint64_t match_number = 0;
    char printable = 'Y';
    Price price = 0;

    [[nodiscard]] constexpr bool is_printable() const noexcept { return printable == 'Y'; }
    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.order_ref);
        v.u32(m.shares);
        v.u64(m.match_number);
        v.ch(m.printable);
        v.u32(m.price);
    }
    friend bool operator==(const OrderExecutedWithPrice&, const OrderExecutedWithPrice&) = default;
};

// 'X' Order Cancel. A partial cancel: `shares` is the amount removed, not the
// amount left.
struct OrderCancel {
    static constexpr char kType = 'X';
    Header hdr{};
    OrderId order_ref = 0;
    Qty shares = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.order_ref);
        v.u32(m.shares);
    }
    friend bool operator==(const OrderCancel&, const OrderCancel&) = default;
};

// 'D' Order Delete. Removes whatever is left of the order.
struct OrderDelete {
    static constexpr char kType = 'D';
    Header hdr{};
    OrderId order_ref = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.order_ref);
    }
    friend bool operator==(const OrderDelete&, const OrderDelete&) = default;
};

// 'U' Order Replace. The original order is removed and a new one, under a new
// reference number, takes its place. The side is not repeated: the new order
// inherits it from the original.
struct OrderReplace {
    static constexpr char kType = 'U';
    Header hdr{};
    OrderId orig_order_ref = 0;
    OrderId new_order_ref = 0;
    Qty shares = 0;
    Price price = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.orig_order_ref);
        v.u64(m.new_order_ref);
        v.u32(m.shares);
        v.u32(m.price);
    }
    friend bool operator==(const OrderReplace&, const OrderReplace&) = default;
};

// ---------------------------------------------------------------------------
// Trade and auction messages. They report prints; the displayed book is
// untouched.
// ---------------------------------------------------------------------------

// 'P' Trade (non-cross). An execution against an order that was never
// displayed, so there is nothing in the book to reduce.
struct Trade {
    static constexpr char kType = 'P';
    Header hdr{};
    OrderId order_ref = 0;
    Side side = Side::Buy;
    Qty shares = 0;
    Symbol stock{};
    Price price = 0;
    std::uint64_t match_number = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.order_ref);
        v.side(m.side);
        v.u32(m.shares);
        v.chars(m.stock.raw);
        v.u32(m.price);
        v.u64(m.match_number);
    }
    friend bool operator==(const Trade&, const Trade&) = default;
};

// 'Q' Cross Trade. The bulk print for an opening, closing or halt cross. Its
// share count is eight bytes, unlike every other share field.
struct CrossTrade {
    static constexpr char kType = 'Q';
    Header hdr{};
    std::uint64_t shares = 0;
    Symbol stock{};
    Price cross_price = 0;
    std::uint64_t match_number = 0;
    char cross_type = ' ';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.shares);
        v.chars(m.stock.raw);
        v.u32(m.cross_price);
        v.u64(m.match_number);
        v.ch(m.cross_type);
    }
    friend bool operator==(const CrossTrade&, const CrossTrade&) = default;
};

// 'B' Broken Trade.
struct BrokenTrade {
    static constexpr char kType = 'B';
    Header hdr{};
    std::uint64_t match_number = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.match_number);
    }
    friend bool operator==(const BrokenTrade&, const BrokenTrade&) = default;
};

// 'I' Net Order Imbalance Indicator.
struct Noii {
    static constexpr char kType = 'I';
    Header hdr{};
    std::uint64_t paired_shares = 0;
    std::uint64_t imbalance_shares = 0;
    char imbalance_direction = ' ';
    Symbol stock{};
    Price far_price = 0;
    Price near_price = 0;
    Price reference_price = 0;
    char cross_type = ' ';
    char price_variation = ' ';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.u64(m.paired_shares);
        v.u64(m.imbalance_shares);
        v.ch(m.imbalance_direction);
        v.chars(m.stock.raw);
        v.u32(m.far_price);
        v.u32(m.near_price);
        v.u32(m.reference_price);
        v.ch(m.cross_type);
        v.ch(m.price_variation);
    }
    friend bool operator==(const Noii&, const Noii&) = default;
};

// 'N' Retail Price Improvement Indicator.
struct Rpii {
    static constexpr char kType = 'N';
    Header hdr{};
    Symbol stock{};
    char interest_flag = ' ';

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.chars(m.stock.raw);
        v.ch(m.interest_flag);
    }
    friend bool operator==(const Rpii&, const Rpii&) = default;
};

// 'O' Direct Listing with Capital Raise Price Discovery.
struct DirectListingCapitalRaise {
    static constexpr char kType = 'O';
    Header hdr{};
    Symbol stock{};
    char open_eligibility = ' ';
    Price min_allowable_price = 0;
    Price max_allowable_price = 0;
    Price near_execution_price = 0;
    std::uint64_t near_execution_time = 0;
    Price lower_collar = 0;
    Price upper_collar = 0;

    [[nodiscard]] constexpr char type() const noexcept { return kType; }
    template <class Self, class V>
    static constexpr void visit(Self& m, V& v) {
        detail::visit_header(m.hdr, v);
        v.chars(m.stock.raw);
        v.ch(m.open_eligibility);
        v.u32(m.min_allowable_price);
        v.u32(m.max_allowable_price);
        v.u32(m.near_execution_price);
        v.u64(m.near_execution_time);
        v.u32(m.lower_collar);
        v.u32(m.upper_collar);
    }
    friend bool operator==(const DirectListingCapitalRaise&,
                           const DirectListingCapitalRaise&) = default;
};

}  // namespace obe::feed
