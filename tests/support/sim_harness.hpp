#pragma once

#include <cstdint>
#include <ostream>
#include <vector>

#include "obe/engine/market_data.hpp"
#include "obe/feed/messages.hpp"
#include "obe/sim/simulator.hpp"
#include "obe/sim/types.hpp"
#include "obe/types.hpp"

// Tools for testing the market-making simulator with messages built by hand.

namespace obe::sim {

// Readable failures. Found by argument-dependent lookup, hence the namespace.
inline void PrintTo(const Quote& q, std::ostream* os) {
    *os << "{" << q.qty << " @ " << q.price << "}";
}
inline void PrintTo(const RestingQuote& q, std::ostream* os) {
    *os << "{" << q.qty << " @ " << q.price << ", " << q.ahead << " ahead}";
}
inline void PrintTo(const Fill& f, std::ostream* os) {
    *os << "{t " << f.time << (f.side == Side::Buy ? ", bought " : ", sold ") << f.qty << " @ "
        << f.price << ", mid2 " << f.mid2 << ", reason " << static_cast<int>(f.reason) << "}";
}

}  // namespace obe::sim

namespace obe::test {

inline constexpr Locate kSimLocate = 7;
inline constexpr Locate kOtherLocate = 8;
inline const feed::Symbol kSimSymbol = feed::Symbol::from("QUOTE");
inline const feed::Symbol kOtherSymbol = feed::Symbol::from("OTHER");

// A price in whole cents: cents(1000) is $10.00.
[[nodiscard]] constexpr Price cents(std::uint32_t c) noexcept {
    return c * 100;
}

// A strategy that answers whatever the test last told it to, and remembers
// every view it was shown.
struct Scripted {
    sim::Quotes want;
    std::vector<sim::MarketView> seen;

    sim::Quotes quote(const sim::MarketView& view) {
        seen.push_back(view);
        return want;
    }

    void bid(Price price, Qty qty = 100) { want.bid = {price, qty}; }
    void ask(Price price, Qty qty = 100) { want.ask = {price, qty}; }
    void no_bid() { want.bid = {}; }
    void no_ask() { want.ask = {}; }
};

// Feeds messages for one security to a simulator, as a feed would. Every
// message is `step` nanoseconds after the one before unless at() says
// otherwise.
template <class Sim>
class Tape {
 public:
    explicit Tape(Sim& sim, Nanos start = 1'000, Nanos step = 1'000)
        : sim_(&sim), now_(start), step_(step) {}

    // Lists both securities and opens them for trading.
    void list() {
        sim_->on_stock_directory(engine::md::directory(kSimLocate, kSimSymbol, tick()));
        sim_->on_trading_action(engine::md::trading(kSimLocate, kSimSymbol, tick()));
        sim_->on_stock_directory(engine::md::directory(kOtherLocate, kOtherSymbol, tick()));
        sim_->on_trading_action(engine::md::trading(kOtherLocate, kOtherSymbol, tick()));
    }

    // The next message will carry exactly this time.
    void at(Nanos time) {
        now_ = time;
        fixed_ = true;
    }
    [[nodiscard]] Nanos now() const noexcept { return now_; }

    OrderId add(Side side, Price price, Qty qty, Locate locate = kSimLocate) {
        const OrderId ref = next_ref_++;
        sim_->on_add(engine::md::add(locate, tick(), ref, side, qty,
                                     locate == kSimLocate ? kSimSymbol : kOtherSymbol, price));
        return ref;
    }
    // An add with a reference number chosen by the test (to reuse a live one).
    void add_as(OrderId ref, Side side, Price price, Qty qty) {
        sim_->on_add(engine::md::add(kSimLocate, tick(), ref, side, qty, kSimSymbol, price));
    }
    void execute(OrderId ref, Qty qty, Locate locate = kSimLocate) {
        sim_->on_execute(engine::md::execute(locate, tick(), ref, qty, ++match_));
    }
    void execute_at(OrderId ref, Qty qty, Price price, bool printable = true) {
        feed::OrderExecutedWithPrice m;
        m.hdr = engine::md::header(kSimLocate, tick());
        m.order_ref = ref;
        m.shares = qty;
        m.match_number = ++match_;
        m.printable = printable ? 'Y' : 'N';
        m.price = price;
        sim_->on_execute_with_price(m);
    }
    void cancel(OrderId ref, Qty qty) {
        sim_->on_cancel(engine::md::reduce(kSimLocate, tick(), ref, qty));
    }
    void remove(OrderId ref) { sim_->on_delete(engine::md::remove(kSimLocate, tick(), ref)); }
    OrderId replace(OrderId old_ref, Price price, Qty qty) {
        const OrderId ref = next_ref_++;
        sim_->on_replace(engine::md::replace(kSimLocate, tick(), old_ref, ref, qty, price));
        return ref;
    }
    void state(char trading_state) {
        feed::TradingAction m = engine::md::trading(kSimLocate, kSimSymbol, tick());
        m.trading_state = trading_state;
        sim_->on_trading_action(m);
    }
    // A trade against a hidden order: never part of the displayed book.
    void hidden_trade(Side side, Price price, Qty qty) {
        feed::Trade m;
        m.hdr = engine::md::header(kSimLocate, tick());
        m.order_ref = 0;
        m.side = side;
        m.shares = qty;
        m.stock = kSimSymbol;
        m.price = price;
        m.match_number = ++match_;
        sim_->on_trade(m);
    }

 private:
    Nanos tick() {
        if (fixed_) {
            fixed_ = false;
        } else {
            now_ += step_;
        }
        return now_;
    }

    Sim* sim_;
    Nanos now_;
    Nanos step_;
    bool fixed_ = false;
    OrderId next_ref_ = 1;
    std::uint64_t match_ = 0;
};

}  // namespace obe::test
