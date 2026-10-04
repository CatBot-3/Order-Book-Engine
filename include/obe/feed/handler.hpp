#pragma once

#include "obe/feed/messages.hpp"

// The interface between the parser and whatever consumes messages.
//
// Dispatch is resolved at compile time. The parser is a template over the
// handler type and calls handler.on_add(msg) and friends directly, so the
// compiler sees the callee and can inline it. With a virtual interface every
// message would cost an indirect call that the optimizer cannot see through,
// and the handler type is known at compile time anyway, so nothing is lost.

namespace obe::feed {

// Empty default for every callback. A handler inherits from this and declares
// only the callbacks it cares about. Its own declarations hide the ones here,
// and because the parser calls through the concrete handler type, no virtual
// dispatch is involved.
struct HandlerBase {
    void on_system_event(const SystemEvent&) noexcept {}
    void on_stock_directory(const StockDirectory&) noexcept {}
    void on_trading_action(const TradingAction&) noexcept {}
    void on_reg_sho(const RegSho&) noexcept {}
    void on_market_participant_position(const MarketParticipantPosition&) noexcept {}
    void on_mwcb_decline_level(const MwcbDeclineLevel&) noexcept {}
    void on_mwcb_status(const MwcbStatus&) noexcept {}
    void on_ipo_quoting_period(const IpoQuotingPeriod&) noexcept {}
    void on_luld_auction_collar(const LuldAuctionCollar&) noexcept {}
    void on_operational_halt(const OperationalHalt&) noexcept {}

    void on_add(const AddOrder&) noexcept {}
    void on_execute(const OrderExecuted&) noexcept {}
    void on_execute_with_price(const OrderExecutedWithPrice&) noexcept {}
    void on_cancel(const OrderCancel&) noexcept {}
    void on_delete(const OrderDelete&) noexcept {}
    void on_replace(const OrderReplace&) noexcept {}

    void on_trade(const Trade&) noexcept {}
    void on_cross_trade(const CrossTrade&) noexcept {}
    void on_broken_trade(const BrokenTrade&) noexcept {}
    void on_noii(const Noii&) noexcept {}
    void on_rpii(const Rpii&) noexcept {}
    void on_direct_listing(const DirectListingCapitalRaise&) noexcept {}
};

// What the parser requires of a handler. Inheriting from HandlerBase satisfies
// it. Spelling it out as a concept means a callback declared with the wrong
// parameter type fails at the parser's declaration with a readable message,
// instead of deep inside the dispatch switch.
//
// What it cannot catch: a misspelled name. `on_ad` in a handler that inherits
// HandlerBase compiles, and adds silently go to the empty default. The scenario
// tests are what catch that.
template <class H>
concept ItchHandler = requires(H& h) {
    h.on_system_event(SystemEvent{});
    h.on_stock_directory(StockDirectory{});
    h.on_trading_action(TradingAction{});
    h.on_reg_sho(RegSho{});
    h.on_market_participant_position(MarketParticipantPosition{});
    h.on_mwcb_decline_level(MwcbDeclineLevel{});
    h.on_mwcb_status(MwcbStatus{});
    h.on_ipo_quoting_period(IpoQuotingPeriod{});
    h.on_luld_auction_collar(LuldAuctionCollar{});
    h.on_operational_halt(OperationalHalt{});
    h.on_add(AddOrder{});
    h.on_execute(OrderExecuted{});
    h.on_execute_with_price(OrderExecutedWithPrice{});
    h.on_cancel(OrderCancel{});
    h.on_delete(OrderDelete{});
    h.on_replace(OrderReplace{});
    h.on_trade(Trade{});
    h.on_cross_trade(CrossTrade{});
    h.on_broken_trade(BrokenTrade{});
    h.on_noii(Noii{});
    h.on_rpii(Rpii{});
    h.on_direct_listing(DirectListingCapitalRaise{});
};

static_assert(ItchHandler<HandlerBase>);

}  // namespace obe::feed
