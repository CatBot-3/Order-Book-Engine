#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/engines.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"
#include "support/printers.hpp"

// What the engine tests share: sinks that remember everything, readable
// failure output, and the list of engines under test.
//
// Every suite in tests/engine is a typed test over EngineTypes and is built
// twice, like the book suites:
//
//   obe_engine_tests                the reference engine. Part of the passing
//                                   suite.
//   obe_hand_written_engine_tests   built with OBE_TEST_HAND_WRITTEN defined:
//                                   MatchingEngine. Labelled needs-your-code.

namespace obe::engine {

// GoogleTest finds these by argument-dependent lookup.

inline const char* name(RejectReason r) {
    switch (r) {
        case RejectReason::ZeroQuantity:
            return "ZeroQuantity";
        case RejectReason::ZeroPrice:
            return "ZeroPrice";
        case RejectReason::BadSide:
            return "BadSide";
        case RejectReason::UnknownInstrument:
            return "UnknownInstrument";
        case RejectReason::UnknownOrder:
            return "UnknownOrder";
        case RejectReason::NotOwner:
            return "NotOwner";
        case RejectReason::BadInstruction:
            return "BadInstruction";
        case RejectReason::WouldTrade:
            return "WouldTrade";
    }
    return "?";
}

inline const char* name(CancelReason r) {
    switch (r) {
        case CancelReason::Requested:
            return "Requested";
        case CancelReason::ImmediateOrCancel:
            return "ImmediateOrCancel";
        case CancelReason::FillOrKill:
            return "FillOrKill";
        case CancelReason::NoLiquidity:
            return "NoLiquidity";
        case CancelReason::PostOnly:
            return "PostOnly";
        case CancelReason::SelfMatch:
            return "SelfMatch";
    }
    return "?";
}

inline void PrintTo(RejectReason r, std::ostream* os) {
    *os << name(r);
}
inline void PrintTo(CancelReason r, std::ostream* os) {
    *os << name(r);
}

inline void PrintTo(const Accepted& r, std::ostream* os) {
    *os << "Accepted{id " << r.order_id << ", owner " << r.owner << ", token " << r.token
        << ", locate " << r.locate << ", " << static_cast<char>(r.side) << " " << r.qty << " @ "
        << r.price << ", kind " << static_cast<int>(r.kind) << ", tif " << static_cast<int>(r.tif)
        << ", t " << r.timestamp << "}";
}

inline void PrintTo(const Executed& r, std::ostream* os) {
    *os << "Executed{id " << r.order_id << ", owner " << r.owner << ", token " << r.token << ", "
        << r.qty << " @ " << r.price << ", leaves " << r.leaves << ", match " << r.match_number
        << ", " << static_cast<char>(r.liquidity) << ", t " << r.timestamp << "}";
}

inline void PrintTo(const Cancelled& r, std::ostream* os) {
    *os << "Cancelled{id " << r.order_id << ", owner " << r.owner << ", token " << r.token
        << ", qty " << r.qty << ", leaves " << r.leaves << ", " << name(r.reason) << ", t "
        << r.timestamp << "}";
}

inline void PrintTo(const Replaced& r, std::ostream* os) {
    *os << "Replaced{" << r.old_id << " -> " << r.new_id << ", owner " << r.owner << ", token "
        << r.token << ", " << r.qty << " @ " << r.price
        << (r.kept_priority ? ", kept priority" : ", lost priority") << ", t " << r.timestamp
        << "}";
}

inline void PrintTo(const Rejected& r, std::ostream* os) {
    *os << "Rejected{owner " << r.owner << ", token " << r.token << ", id " << r.order_id << ", "
        << name(r.reason) << ", t " << r.timestamp << "}";
}

inline void PrintTo(const RestingOrder& o, std::ostream* os) {
    *os << "{id " << o.id << ", owner " << o.owner << ", token " << o.token << ", " << o.qty
        << " @ " << o.price;
    if (o.hidden != 0 || o.display != 0) {
        *os << ", hidden " << o.hidden << ", display " << o.display;
    }
    if (o.ref != 0) {
        *os << ", ref " << o.ref;
    }
    if (o.post_only) {
        *os << ", post-only";
    }
    if (o.self_match != SelfMatch::Allow) {
        *os << ", self-match " << static_cast<int>(o.self_match);
    }
    *os << "}";
}

inline void PrintTo(const EngineStats& s, std::ostream* os) {
    *os << "{accepted " << s.accepted << ", rejected " << s.rejected << ", cancels " << s.cancels
        << ", replaces " << s.replaces << ", trades " << s.trades << ", traded " << s.traded_shares
        << ", unfilled " << s.unfilled_shares << ", self-matches " << s.self_matches << "}";
}

}  // namespace obe::engine

namespace obe::feed {

inline void PrintTo(const AddOrder& m, std::ostream* os) {
    *os << "A{t " << m.hdr.timestamp << ", locate " << m.hdr.locate << ", ref " << m.order_ref
        << ", " << static_cast<char>(m.side) << " " << m.shares << " " << m.stock.view() << " @ "
        << m.price << "}";
}
inline void PrintTo(const OrderExecuted& m, std::ostream* os) {
    *os << "E{t " << m.hdr.timestamp << ", locate " << m.hdr.locate << ", ref " << m.order_ref
        << ", " << m.shares << ", match " << m.match_number << "}";
}
inline void PrintTo(const OrderCancel& m, std::ostream* os) {
    *os << "X{t " << m.hdr.timestamp << ", locate " << m.hdr.locate << ", ref " << m.order_ref
        << ", " << m.shares << "}";
}
inline void PrintTo(const OrderDelete& m, std::ostream* os) {
    *os << "D{t " << m.hdr.timestamp << ", locate " << m.hdr.locate << ", ref " << m.order_ref
        << "}";
}
inline void PrintTo(const OrderReplace& m, std::ostream* os) {
    *os << "U{t " << m.hdr.timestamp << ", locate " << m.hdr.locate << ", " << m.orig_order_ref
        << " -> " << m.new_order_ref << ", " << m.shares << " @ " << m.price << "}";
}
inline void PrintTo(const StockDirectory& m, std::ostream* os) {
    *os << "R{t " << m.hdr.timestamp << ", locate " << m.hdr.locate << ", " << m.stock.view()
        << "}";
}
inline void PrintTo(const TradingAction& m, std::ostream* os) {
    *os << "H{t " << m.hdr.timestamp << ", locate " << m.hdr.locate << ", " << m.stock.view()
        << ", state " << m.trading_state << "}";
}

}  // namespace obe::feed

namespace obe::test {

#if defined(OBE_TEST_HAND_WRITTEN)
using EngineTypes = ::testing::Types<engine::PooledEngineImpl>;
#else
using EngineTypes = ::testing::Types<engine::ReferenceEngineImpl>;
#endif

using Report = std::variant<engine::Accepted, engine::Executed, engine::Cancelled, engine::Replaced,
                            engine::Rejected>;

// A report sink that keeps every report, in order.
struct ReportLog {
    std::vector<Report> all;

    void on_accepted(const engine::Accepted& r) { all.emplace_back(r); }
    void on_executed(const engine::Executed& r) { all.emplace_back(r); }
    void on_cancelled(const engine::Cancelled& r) { all.emplace_back(r); }
    void on_replaced(const engine::Replaced& r) { all.emplace_back(r); }
    void on_rejected(const engine::Rejected& r) { all.emplace_back(r); }

    // Every report of one type, in order.
    template <class T>
    [[nodiscard]] std::vector<T> of() const {
        std::vector<T> out;
        for (const Report& r : all) {
            if (const T* p = std::get_if<T>(&r)) {
                out.push_back(*p);
            }
        }
        return out;
    }

    void clear() { all.clear(); }
};

using MdMessage =
    std::variant<feed::StockDirectory, feed::TradingAction, feed::AddOrder, feed::OrderExecuted,
                 feed::OrderCancel, feed::OrderDelete, feed::OrderReplace>;

// A market-data sink that keeps every message, in order.
struct MdLog {
    std::vector<MdMessage> all;

    void on_stock_directory(const feed::StockDirectory& m) { all.emplace_back(m); }
    void on_trading_action(const feed::TradingAction& m) { all.emplace_back(m); }
    void on_add(const feed::AddOrder& m) { all.emplace_back(m); }
    void on_execute(const feed::OrderExecuted& m) { all.emplace_back(m); }
    void on_cancel(const feed::OrderCancel& m) { all.emplace_back(m); }
    void on_delete(const feed::OrderDelete& m) { all.emplace_back(m); }
    void on_replace(const feed::OrderReplace& m) { all.emplace_back(m); }

    // The type byte of every message, in order: "AAEED" and so on.
    [[nodiscard]] std::string trace() const {
        std::string out;
        for (const MdMessage& m : all) {
            out.push_back(std::visit([](const auto& msg) { return msg.type(); }, m));
        }
        return out;
    }

    template <class T>
    [[nodiscard]] std::vector<T> of() const {
        std::vector<T> out;
        for (const MdMessage& m : all) {
            if (const T* p = std::get_if<T>(&m)) {
                out.push_back(*p);
            }
        }
        return out;
    }

    void clear() { all.clear(); }
};

static_assert(engine::ReportSink<ReportLog>);
static_assert(engine::MarketDataSink<MdLog>);

// The levels of one side, best first.
template <class Engine>
[[nodiscard]] std::vector<book::Level> levels_of(const Engine& engine, Locate locate, Side side) {
    std::vector<book::Level> out;
    engine.for_each_level(locate, side, [&out](const book::Level& level) {
        out.push_back(level);
        return true;
    });
    return out;
}

// The orders of one side, in the order they would trade.
template <class Engine>
[[nodiscard]] std::vector<engine::RestingOrder> orders_of(const Engine& engine, Locate locate,
                                                          Side side) {
    std::vector<engine::RestingOrder> out;
    engine.for_each_order(locate, side, [&out](const engine::RestingOrder& order) {
        out.push_back(order);
        return true;
    });
    return out;
}

}  // namespace obe::test
