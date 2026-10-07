#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"

// A second, independent matching engine, used only as an oracle.
//
// The reference engine is the judge of the hand-written one, so something has
// to judge the reference. This does, the same way NaiveBook judges the
// reference book: it implements the contract in obe/engine/concepts.hpp with
// no data structure at all. Every resting order of every instrument sits in
// one vector, in the order it joined the book. Finding who trades next is a
// scan of the whole vector for the best price, and position in the vector is
// time priority.
//
// It shares no code with the reference engine beyond the message builders, and
// none of its ideas: no levels, no queues, no index. A mistake would have to
// be made twice, in two different shapes, to get past both. It is far too slow
// for anything but tests.

namespace obe::test {

template <engine::ReportSink Reports, engine::MarketDataSink MarketData>
class NaiveEngine {
 public:
    NaiveEngine(Reports& reports, MarketData& market_data) : reports_(reports), md_(market_data) {}

    bool add_instrument(Locate locate, const feed::Symbol& symbol, Nanos now) {
        if (symbols_.contains(locate)) {
            return false;
        }
        symbols_.emplace(locate, symbol);
        md_.on_stock_directory(engine::md::directory(locate, symbol, now));
        md_.on_trading_action(engine::md::trading(locate, symbol, now));
        return true;
    }

    OrderId submit(const engine::NewOrder& o, Nanos now) {
        const bool limit = o.kind == engine::OrderKind::Limit;
        const bool rests = limit && o.tif == engine::TimeInForce::Day;
        std::optional<engine::RejectReason> why;
        if (!symbols_.contains(o.locate)) {
            why = engine::RejectReason::UnknownInstrument;
        } else if (o.side != Side::Buy && o.side != Side::Sell) {
            why = engine::RejectReason::BadSide;
        } else if (o.qty == 0) {
            why = engine::RejectReason::ZeroQuantity;
        } else if (limit && o.price == 0) {
            why = engine::RejectReason::ZeroPrice;
        } else if (!rests && (o.post_only || (o.display > 0 && o.display < o.qty))) {
            why = engine::RejectReason::BadInstruction;
        }
        if (why) {
            refuse(o.owner, o.token, 0, *why, now);
            return 0;
        }

        Order order{};
        order.id = ++ids_;
        order.ref = order.id;
        order.owner = o.owner;
        order.token = o.token;
        order.locate = o.locate;
        order.side = o.side;
        order.price = limit ? o.price : Price{0};
        order.qty = o.qty;
        order.display = rests ? o.display : Qty{0};
        order.post_only = o.post_only;
        order.prevention = mode(o.self_match);
        ++stats_.accepted;
        reports_.on_accepted(
            {order.id, o.owner, o.token, o.locate, o.side, o.qty, order.price, o.kind, o.tif, now});

        if (o.post_only && std::any_of(resting_.begin(), resting_.end(),
                                       [&](const Order& r) { return tradable(order, limit, r); })) {
            unfilled(order, engine::CancelReason::PostOnly, now);
            return order.id;
        }
        if (o.tif == engine::TimeInForce::FillOrKill && !would_fill(order, limit)) {
            unfilled(order, engine::CancelReason::FillOrKill, now);
            return order.id;
        }

        if (trade(order, limit, now) == Outcome::Stopped || order.qty == 0) {
            return order.id;
        }
        if (rests) {
            show(order);
            resting_.push_back(order);
            md_.on_add(engine::md::add(order.locate, now, order.ref, order.side, order.qty,
                                       symbols_.at(order.locate), order.price));
        } else {
            unfilled(
                order,
                limit ? engine::CancelReason::ImmediateOrCancel : engine::CancelReason::NoLiquidity,
                now);
        }
        return order.id;
    }

    bool cancel(engine::OwnerId owner, OrderId id, Nanos now) {
        const auto it = find(id);
        if (it == resting_.end()) {
            refuse(owner, 0, id, engine::RejectReason::UnknownOrder, now);
            return false;
        }
        if (it->owner != owner) {
            refuse(owner, 0, id, engine::RejectReason::NotOwner, now);
            return false;
        }
        const Order gone = *it;
        resting_.erase(it);
        ++stats_.cancels;
        md_.on_delete(engine::md::remove(gone.locate, now, gone.ref));
        reports_.on_cancelled({id, owner, gone.token, gone.qty + gone.hidden, 0,
                               engine::CancelReason::Requested, now});
        return true;
    }

    OrderId replace(engine::OwnerId owner, OrderId id, Qty new_qty, Price new_price, Nanos now) {
        const auto it = find(id);
        std::optional<engine::RejectReason> why;
        if (it == resting_.end()) {
            why = engine::RejectReason::UnknownOrder;
        } else if (it->owner != owner) {
            why = engine::RejectReason::NotOwner;
        } else if (new_qty == 0) {
            why = engine::RejectReason::ZeroQuantity;
        } else if (new_price == 0) {
            why = engine::RejectReason::ZeroPrice;
        } else if (it->post_only) {
            Order moved = *it;
            moved.price = new_price;
            if (std::any_of(resting_.begin(), resting_.end(),
                            [&](const Order& r) { return tradable(moved, true, r); })) {
                why = engine::RejectReason::WouldTrade;
            }
        }
        if (why) {
            refuse(owner, 0, id, *why, now);
            return 0;
        }
        ++stats_.replaces;

        const Qty open = it->qty + it->hidden;
        if (new_price == it->price && new_qty <= open) {
            // The reserve gives way first; the market hears only about what
            // comes off the part it could see.
            Qty to_remove = open - new_qty;
            const Qty unseen = std::min(to_remove, it->hidden);
            it->hidden -= unseen;
            to_remove -= unseen;
            if (to_remove != 0) {
                md_.on_cancel(engine::md::reduce(it->locate, now, it->ref, to_remove));
                it->qty -= to_remove;
            }
            reports_.on_replaced({id, id, owner, it->token, new_qty, new_price, true, now});
            return id;
        }

        Order order = *it;
        const OrderId old_ref = it->ref;
        resting_.erase(it);
        order.id = ++ids_;
        order.ref = order.id;
        order.qty = new_qty;
        order.hidden = 0;
        order.price = new_price;
        reports_.on_replaced({id, order.id, owner, order.token, new_qty, new_price, false, now});

        const bool trades = std::any_of(resting_.begin(), resting_.end(),
                                        [&](const Order& r) { return tradable(order, true, r); });
        const OrderId new_id = order.id;
        if (!trades) {
            show(order);
            md_.on_replace(
                engine::md::replace(order.locate, now, old_ref, order.id, order.qty, new_price));
            resting_.push_back(order);
            return new_id;
        }
        md_.on_delete(engine::md::remove(order.locate, now, old_ref));
        if (trade(order, true, now) == Outcome::Finished && order.qty != 0) {
            show(order);
            resting_.push_back(order);
            md_.on_add(engine::md::add(order.locate, now, order.ref, order.side, order.qty,
                                       symbols_.at(order.locate), order.price));
        }
        return new_id;
    }

    [[nodiscard]] std::optional<book::Level> best(Locate locate, Side side) const {
        std::optional<book::Level> out;
        for_each_level(locate, side, [&out](const book::Level& level) {
            out = level;
            return false;
        });
        return out;
    }

    template <class F>
    void for_each_level(Locate locate, Side side, F&& f) const {
        std::optional<book::Level> level;
        for (const Order& o : sorted(locate, side)) {
            if (level && level->price != o.price) {
                if (!f(*level)) {
                    return;
                }
                level.reset();
            }
            if (!level) {
                level = book::Level{o.price, 0};
            }
            level->qty += o.qty;
        }
        if (level) {
            f(*level);
        }
    }

    template <class F>
    void for_each_order(Locate locate, Side side, F&& f) const {
        for (const Order& o : sorted(locate, side)) {
            engine::RestingOrder shown{o.id, o.owner, o.token, o.price, o.qty};
            shown.hidden = o.hidden;
            shown.display = o.display;
            shown.ref = o.ref == o.id ? OrderId{0} : o.ref;
            shown.post_only = o.post_only;
            shown.self_match = o.prevention;
            if (!f(shown)) {
                return;
            }
        }
    }

    [[nodiscard]] std::size_t open_orders() const noexcept { return resting_.size(); }
    [[nodiscard]] bool listed(Locate locate) const { return symbols_.contains(locate); }
    [[nodiscard]] const engine::EngineStats& stats() const noexcept { return stats_; }

 private:
    // While an order is on its way in, `qty` is everything it has left. Once
    // it rests, `qty` is what the market can see and `hidden` is the rest.
    struct Order {
        OrderId id;
        OrderId ref;  // its name in the market data
        engine::OwnerId owner;
        engine::Token token;
        Locate locate;
        Side side;
        Price price;
        Qty qty;
        Qty hidden;
        Qty display;
        bool post_only;
        engine::SelfMatch prevention;
    };

    enum class Outcome : std::uint8_t {
        Finished,  // filled, or nothing left to trade with
        Stopped,   // self-match prevention cancelled what was left
    };

    [[nodiscard]] static engine::SelfMatch mode(engine::SelfMatch asked) {
        switch (asked) {
            case engine::SelfMatch::CancelIncoming:
            case engine::SelfMatch::CancelResting:
            case engine::SelfMatch::CancelBoth:
                return asked;
            default:
                return engine::SelfMatch::Allow;
        }
    }

    // Splits an order that is about to rest into what is shown and what is not.
    static void show(Order& order) {
        const Qty all = order.qty;
        order.qty = order.display != 0 && order.display < all ? order.display : all;
        order.hidden = all - order.qty;
    }

    [[nodiscard]] auto find(OrderId id) {
        return std::find_if(resting_.begin(), resting_.end(),
                            [id](const Order& o) { return o.id == id; });
    }

    // Whether the incoming order may trade with the resting order `r`.
    [[nodiscard]] static bool tradable(const Order& in, bool has_limit, const Order& r) {
        if (r.locate != in.locate || r.side == in.side) {
            return false;
        }
        if (!has_limit) {
            return true;
        }
        return in.side == Side::Buy ? r.price <= in.price : r.price >= in.price;
    }

    // True if resting order `a` should trade before resting order `b`, both on
    // the same side. Only price is compared: the scan below keeps the earlier
    // of two equally priced orders by never replacing on a tie.
    [[nodiscard]] static bool better_priced(const Order& a, const Order& b) {
        return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
    }

    // Whether a fill-or-kill order would be filled. There is no cleverness
    // here on purpose: the trading is simply done, on this engine with its
    // output switched off, and then undone. Whatever the rules for icebergs
    // and self-match prevention imply for fill-or-kill, this gets it by
    // construction, which is what makes it a check on the reference engine's
    // reasoning about the same question.
    [[nodiscard]] bool would_fill(const Order& order, bool has_limit) {
        const std::vector<Order> saved_book = resting_;
        const engine::EngineStats saved_stats = stats_;
        const OrderId saved_ids = ids_;
        const std::uint64_t saved_matches = matches_;
        quiet_ = true;
        Order trial = order;
        const bool filled = trade(trial, has_limit, 0) == Outcome::Finished && trial.qty == 0;
        quiet_ = false;
        resting_ = saved_book;
        stats_ = saved_stats;
        ids_ = saved_ids;
        matches_ = saved_matches;
        return filled;
    }

    Outcome trade(Order& in, bool has_limit, Nanos now) {
        while (in.qty != 0) {
            std::size_t pick = resting_.size();
            for (std::size_t i = 0; i < resting_.size(); ++i) {
                if (!tradable(in, has_limit, resting_[i])) {
                    continue;
                }
                if (pick == resting_.size() || better_priced(resting_[i], resting_[pick])) {
                    pick = i;
                }
            }
            if (pick == resting_.size()) {
                return Outcome::Finished;
            }

            if (in.prevention != engine::SelfMatch::Allow && resting_[pick].owner == in.owner) {
                ++stats_.self_matches;
                if (in.prevention != engine::SelfMatch::CancelIncoming) {
                    const Order gone = resting_[pick];
                    resting_.erase(resting_.begin() + static_cast<std::ptrdiff_t>(pick));
                    if (!quiet_) {
                        md_.on_delete(engine::md::remove(gone.locate, now, gone.ref));
                        reports_.on_cancelled({gone.id, gone.owner, gone.token,
                                               gone.qty + gone.hidden, 0,
                                               engine::CancelReason::SelfMatch, now});
                    }
                }
                if (in.prevention != engine::SelfMatch::CancelResting) {
                    if (!quiet_) {
                        unfilled(in, engine::CancelReason::SelfMatch, now);
                    }
                    return Outcome::Stopped;
                }
                continue;
            }

            Order& r = resting_[pick];
            const Qty shares = std::min(in.qty, r.qty);
            const std::uint64_t match = ++matches_;
            r.qty -= shares;
            in.qty -= shares;
            ++stats_.trades;
            stats_.traded_shares += shares;
            if (!quiet_) {
                md_.on_execute(engine::md::execute(in.locate, now, r.ref, shares, match));
                reports_.on_executed({r.id, r.owner, r.token, shares, r.price, r.qty + r.hidden,
                                      match, engine::Liquidity::Added, now});
                reports_.on_executed({in.id, in.owner, in.token, shares, r.price, in.qty, match,
                                      engine::Liquidity::Removed, now});
            }
            if (r.qty != 0) {
                continue;
            }
            // Nothing of it is showing. An iceberg shows its next slice, as if
            // it were an order that had just arrived: last in time, and under
            // a reference the market has not seen. Anything else is done.
            Order next = r;
            resting_.erase(resting_.begin() + static_cast<std::ptrdiff_t>(pick));
            if (next.hidden != 0) {
                next.qty = std::min(next.display, next.hidden);
                next.hidden -= next.qty;
                next.ref = ++ids_;
                resting_.push_back(next);
                if (!quiet_) {
                    md_.on_add(engine::md::add(next.locate, now, next.ref, next.side, next.qty,
                                               symbols_.at(next.locate), next.price));
                }
            }
        }
        return Outcome::Finished;
    }

    void unfilled(const Order& order, engine::CancelReason reason, Nanos now) {
        stats_.unfilled_shares += order.qty;
        reports_.on_cancelled({order.id, order.owner, order.token, order.qty, 0, reason, now});
    }

    void refuse(engine::OwnerId owner, engine::Token token, OrderId id, engine::RejectReason reason,
                Nanos now) {
        ++stats_.rejected;
        reports_.on_rejected({owner, token, id, reason, now});
    }

    // One side of one book in trading order. The sort is stable, so orders at
    // a price stay in the order they joined.
    [[nodiscard]] std::vector<Order> sorted(Locate locate, Side side) const {
        std::vector<Order> out;
        for (const Order& o : resting_) {
            if (o.locate == locate && o.side == side) {
                out.push_back(o);
            }
        }
        std::stable_sort(out.begin(), out.end(), better_priced);
        return out;
    }

    Reports& reports_;
    MarketData& md_;
    std::vector<Order> resting_;  // every resting order, oldest in the queue first
    std::map<Locate, feed::Symbol> symbols_;
    engine::EngineStats stats_{};
    OrderId ids_ = 0;
    std::uint64_t matches_ = 0;
    bool quiet_ = false;  // a trial run for fill-or-kill: change the book, say nothing
};

struct NaiveEngineImpl {
    static constexpr std::string_view kName = "naive";
    template <engine::ReportSink R, engine::MarketDataSink M>
    using Engine = NaiveEngine<R, M>;
};

static_assert(engine::EngineLike<NaiveEngine<engine::NullReports, engine::NullMarketData>>);

}  // namespace obe::test
