#pragma once

#include <string>

#include "obe/feed/handler.hpp"

namespace obe::test {

// Records the type byte of every message it is given, in order. A stream that
// decodes as System Event, Add, Execute leaves trace == "SAE".
struct TraceHandler : feed::HandlerBase {
    std::string trace;

    template <class M>
    void note(const M& m) {
        trace.push_back(m.type());
    }

    void on_system_event(const feed::SystemEvent& m) { note(m); }
    void on_stock_directory(const feed::StockDirectory& m) { note(m); }
    void on_trading_action(const feed::TradingAction& m) { note(m); }
    void on_reg_sho(const feed::RegSho& m) { note(m); }
    void on_market_participant_position(const feed::MarketParticipantPosition& m) { note(m); }
    void on_mwcb_decline_level(const feed::MwcbDeclineLevel& m) { note(m); }
    void on_mwcb_status(const feed::MwcbStatus& m) { note(m); }
    void on_ipo_quoting_period(const feed::IpoQuotingPeriod& m) { note(m); }
    void on_luld_auction_collar(const feed::LuldAuctionCollar& m) { note(m); }
    void on_operational_halt(const feed::OperationalHalt& m) { note(m); }
    void on_add(const feed::AddOrder& m) { note(m); }
    void on_execute(const feed::OrderExecuted& m) { note(m); }
    void on_execute_with_price(const feed::OrderExecutedWithPrice& m) { note(m); }
    void on_cancel(const feed::OrderCancel& m) { note(m); }
    void on_delete(const feed::OrderDelete& m) { note(m); }
    void on_replace(const feed::OrderReplace& m) { note(m); }
    void on_trade(const feed::Trade& m) { note(m); }
    void on_cross_trade(const feed::CrossTrade& m) { note(m); }
    void on_broken_trade(const feed::BrokenTrade& m) { note(m); }
    void on_noii(const feed::Noii& m) { note(m); }
    void on_rpii(const feed::Rpii& m) { note(m); }
    void on_direct_listing(const feed::DirectListingCapitalRaise& m) { note(m); }
};

}  // namespace obe::test
