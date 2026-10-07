#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"

// The reference matching engine: the contract in concepts.hpp, written with
// the most obvious containers.
//
//   per instrument, per side   std::map<Price, Queue>           the levels
//   Queue                      std::list of resting orders      oldest first
//   over all instruments       std::unordered_map<OrderId, ..>  where each order is
//
// It allocates a list node for every order that rests and a tree node for
// every new price, and it chases a pointer for nearly everything it does. That
// is accepted here. Its job is to be easy to read and hard to get wrong, so
// that it can be the judge of the hand-written MatchingEngine, exactly as the
// reference book judges the optimized containers in phase 4.
//
// Sinks are held by reference and must outlive the engine. A sink must not
// call back into the engine from inside a callback.

namespace obe::engine {

template <ReportSink Reports, MarketDataSink MarketData>
class ReferenceEngine {
 public:
    static constexpr std::size_t kLocates = std::size_t{1} << 16;

    ReferenceEngine(Reports& reports, MarketData& market_data)
        : reports_(&reports), md_(&market_data), books_(kLocates) {}

    // --- Operations ----------------------------------------------------------

    bool add_instrument(Locate locate, const feed::Symbol& symbol, Nanos now) {
        Book& book = books_[locate];
        if (book.listed) {
            return false;
        }
        book.listed = true;
        book.symbol = symbol;
        md_->on_stock_directory(md::directory(locate, symbol, now));
        md_->on_trading_action(md::trading(locate, symbol, now));
        return true;
    }

    OrderId submit(const NewOrder& order, Nanos now) {
        Book& book = books_[order.locate];
        if (!book.listed) {
            return reject(order.owner, order.token, 0, RejectReason::UnknownInstrument, now);
        }
        if (!valid_side(order.side)) {
            return reject(order.owner, order.token, 0, RejectReason::BadSide, now);
        }
        if (order.qty == 0) {
            return reject(order.owner, order.token, 0, RejectReason::ZeroQuantity, now);
        }
        if (order.kind == OrderKind::Limit && order.price == 0) {
            return reject(order.owner, order.token, 0, RejectReason::ZeroPrice, now);
        }
        const bool is_limit = order.kind == OrderKind::Limit;
        const bool can_rest = is_limit && order.tif == TimeInForce::Day;
        const bool iceberg = order.display != 0 && order.display < order.qty;
        if ((order.post_only || iceberg) && !can_rest) {
            return reject(order.owner, order.token, 0, RejectReason::BadInstruction, now);
        }

        const OrderId id = ++last_id_;
        ++stats_.accepted;
        reports_->on_accepted(Accepted{.order_id = id,
                                       .owner = order.owner,
                                       .token = order.token,
                                       .locate = order.locate,
                                       .side = order.side,
                                       .qty = order.qty,
                                       .price = is_limit ? order.price : Price{0},
                                       .kind = order.kind,
                                       .tif = order.tif,
                                       .timestamp = now});

        const Price limit = effective_limit(order);
        const Incoming incoming{.id = id,
                                .owner = order.owner,
                                .token = order.token,
                                .locate = order.locate,
                                .side = order.side,
                                .display = order.display,
                                .post_only = order.post_only,
                                .self_match = known(order.self_match)};

        if (order.post_only && would_trade(book, order.side, order.price)) {
            drop(incoming, order.qty, CancelReason::PostOnly, now);
            return id;
        }
        if (order.tif == TimeInForce::FillOrKill && !can_fill(book, incoming, limit, order.qty)) {
            drop(incoming, order.qty, CancelReason::FillOrKill, now);
            return id;
        }

        // Zero is also what match() returns when self-match prevention has
        // stopped the order and cancelled what was left of it.
        const Qty left = match(book, incoming, limit, order.qty, now);
        if (left == 0) {
            return id;
        }
        if (can_rest) {
            const Qty shown = rest(book, incoming, order.price, left);
            md_->on_add(
                md::add(order.locate, now, id, order.side, shown, book.symbol, order.price));
        } else {
            drop(incoming, left,
                 is_limit ? CancelReason::ImmediateOrCancel : CancelReason::NoLiquidity, now);
        }
        return id;
    }

    bool cancel(OwnerId owner, OrderId id, Nanos now) {
        const auto found = index_.find(id);
        if (found == index_.end()) {
            reject(owner, 0, id, RejectReason::UnknownOrder, now);
            return false;
        }
        if (found->second.at->owner != owner) {
            reject(owner, 0, id, RejectReason::NotOwner, now);
            return false;
        }
        const Locate locate = found->second.locate;
        const Resting order = unlink(found);
        ++stats_.cancels;
        md_->on_delete(md::remove(locate, now, order.ref));
        reports_->on_cancelled(Cancelled{.order_id = id,
                                         .owner = owner,
                                         .token = order.token,
                                         .qty = order.qty + order.hidden,
                                         .leaves = 0,
                                         .reason = CancelReason::Requested,
                                         .timestamp = now});
        return true;
    }

    OrderId replace(OwnerId owner, OrderId id, Qty new_qty, Price new_price, Nanos now) {
        const auto found = index_.find(id);
        if (found == index_.end()) {
            return reject(owner, 0, id, RejectReason::UnknownOrder, now);
        }
        if (found->second.at->owner != owner) {
            return reject(owner, 0, id, RejectReason::NotOwner, now);
        }
        if (new_qty == 0) {
            return reject(owner, 0, id, RejectReason::ZeroQuantity, now);
        }
        if (new_price == 0) {
            return reject(owner, 0, id, RejectReason::ZeroPrice, now);
        }
        const Locator where = found->second;
        Book& book = books_[where.locate];
        if (where.at->post_only && would_trade(book, where.side, new_price)) {
            return reject(owner, 0, id, RejectReason::WouldTrade, now);
        }
        ++stats_.replaces;

        // Only the size changes, and not upwards: the order stays where it is.
        // The shares come off the part nobody can see first.
        if (new_price == where.price && new_qty <= where.at->qty + where.at->hidden) {
            const Qty removed = where.at->qty + where.at->hidden - new_qty;
            const Qty from_hidden = std::min(removed, where.at->hidden);
            const Qty from_shown = removed - from_hidden;
            Queue& queue = book.side(where.side).find(where.price)->second;
            where.at->hidden -= from_hidden;
            queue.hidden -= from_hidden;
            if (from_shown != 0) {
                where.at->qty -= from_shown;
                queue.total -= from_shown;
                md_->on_cancel(md::reduce(where.locate, now, where.at->ref, from_shown));
            }
            reports_->on_replaced(Replaced{.old_id = id,
                                           .new_id = id,
                                           .owner = owner,
                                           .token = where.at->token,
                                           .qty = new_qty,
                                           .price = new_price,
                                           .kept_priority = true,
                                           .timestamp = now});
            return id;
        }

        // Anything else is a new order: out of the book, then back in at the
        // end of the queue under a new id.
        const Resting old = unlink(found);
        const OrderId new_id = ++last_id_;
        const Incoming incoming{.id = new_id,
                                .owner = owner,
                                .token = old.token,
                                .locate = where.locate,
                                .side = where.side,
                                .display = old.display,
                                .post_only = old.post_only,
                                .self_match = old.self_match};
        reports_->on_replaced(Replaced{.old_id = id,
                                       .new_id = new_id,
                                       .owner = owner,
                                       .token = old.token,
                                       .qty = new_qty,
                                       .price = new_price,
                                       .kept_priority = false,
                                       .timestamp = now});

        if (!would_trade(book, where.side, new_price)) {
            const Qty shown = rest(book, incoming, new_price, new_qty);
            md_->on_replace(md::replace(where.locate, now, old.ref, new_id, shown, new_price));
            return new_id;
        }
        // The market never saw an order that trades on arrival, so there is
        // nothing a Replace message could point its new reference at. The old
        // order is deleted, and what survives the trades is added afresh.
        md_->on_delete(md::remove(where.locate, now, old.ref));
        const Qty left = match(book, incoming, new_price, new_qty, now);
        if (left != 0) {
            const Qty shown = rest(book, incoming, new_price, left);
            md_->on_add(
                md::add(where.locate, now, new_id, where.side, shown, book.symbol, new_price));
        }
        return new_id;
    }

    // --- Queries -------------------------------------------------------------

    [[nodiscard]] std::optional<book::Level> best(Locate locate, Side side) const {
        const Ladder& ladder = books_[locate].side(side);
        if (ladder.empty()) {
            return std::nullopt;
        }
        const auto level = side == Side::Buy ? std::prev(ladder.end()) : ladder.begin();
        return book::Level{level->first, level->second.total};
    }

    template <class F>
    void for_each_level(Locate locate, Side side, F&& f) const {
        walk(books_[locate].side(side), side,
             [&f](Price price, const Queue& queue) { return f(book::Level{price, queue.total}); });
    }

    template <class F>
    void for_each_order(Locate locate, Side side, F&& f) const {
        walk(books_[locate].side(side), side, [&f](Price price, const Queue& queue) {
            for (const Resting& order : queue.orders) {
                if (!f(RestingOrder{.id = order.id,
                                    .owner = order.owner,
                                    .token = order.token,
                                    .price = price,
                                    .qty = order.qty,
                                    .hidden = order.hidden,
                                    .display = order.display,
                                    .ref = order.ref == order.id ? OrderId{0} : order.ref,
                                    .post_only = order.post_only,
                                    .self_match = order.self_match})) {
                    return false;
                }
            }
            return true;
        });
    }

    [[nodiscard]] std::size_t open_orders() const noexcept { return index_.size(); }
    [[nodiscard]] bool listed(Locate locate) const noexcept { return books_[locate].listed; }
    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }

    // --- Restoring (see "Restoring" in concepts.hpp) --------------------------

    [[nodiscard]] feed::Symbol symbol(Locate locate) const noexcept {
        return books_[locate].symbol;
    }
    [[nodiscard]] EngineCounters counters() const noexcept { return {last_id_, last_match_}; }

    bool restore_instrument(Locate locate, const feed::Symbol& symbol) {
        Book& book = books_[locate];
        if (book.listed) {
            return false;
        }
        book.listed = true;
        book.symbol = symbol;
        return true;
    }

    bool restore_order(Locate locate, Side side, const RestingOrder& order) {
        Book& book = books_[locate];
        if (!book.listed || !valid_side(side) || order.qty == 0 || order.price == 0 ||
            index_.contains(order.id)) {
            return false;
        }
        Queue& queue = book.side(side)[order.price];
        queue.orders.push_back(Resting{.id = order.id,
                                       .owner = order.owner,
                                       .token = order.token,
                                       .qty = order.qty,
                                       .hidden = order.hidden,
                                       .display = order.display,
                                       .ref = order.market_ref(),
                                       .post_only = order.post_only,
                                       .self_match = known(order.self_match)});
        queue.total += order.qty;
        queue.hidden += order.hidden;
        index_.emplace(order.id, Locator{locate, side, order.price, std::prev(queue.orders.end())});
        return true;
    }

    void restore_counters(const EngineCounters& counters, const EngineStats& stats) noexcept {
        last_id_ = counters.last_order_id;
        last_match_ = counters.last_match_number;
        stats_ = stats;
    }

 private:
    struct Resting {
        OrderId id;
        OwnerId owner;
        Token token;
        Qty qty;      // displayed
        Qty hidden;   // an iceberg's reserve
        Qty display;  // the display size it was given; 0 shows everything
        OrderId ref;  // what the market data calls it; the id until replenished
        bool post_only;
        SelfMatch self_match;
    };

    // One price level: its orders, oldest at the front, and their totals.
    struct Queue {
        std::list<Resting> orders;
        std::uint64_t total = 0;   // displayed shares: what the market sees
        std::uint64_t hidden = 0;  // reserve shares, which fill-or-kill may count
    };

    using Ladder = std::map<Price, Queue>;

    struct Book {
        Ladder bids;
        Ladder asks;
        feed::Symbol symbol{};
        bool listed = false;

        [[nodiscard]] Ladder& side(Side s) noexcept { return s == Side::Buy ? bids : asks; }
        [[nodiscard]] const Ladder& side(Side s) const noexcept {
            return s == Side::Buy ? bids : asks;
        }
    };

    // Where a resting order is. std::list iterators stay valid while other
    // elements come and go, which is what lets a cancel go straight to its
    // order instead of searching the level for it.
    struct Locator {
        Locate locate;
        Side side;
        Price price;
        typename std::list<Resting>::iterator at;
    };

    using Index = std::unordered_map<OrderId, Locator>;

    // An order on its way in: everything a trade needs to report about it.
    struct Incoming {
        OrderId id;
        OwnerId owner;
        Token token;
        Locate locate;
        Side side;
        Qty display;
        bool post_only;
        SelfMatch self_match;
    };

    // Anything that is not one of the three prevention modes means Allow.
    [[nodiscard]] static constexpr SelfMatch known(SelfMatch mode) noexcept {
        return mode == SelfMatch::CancelIncoming || mode == SelfMatch::CancelResting ||
                       mode == SelfMatch::CancelBoth
                   ? mode
                   : SelfMatch::Allow;
    }

    // Calls f(price, queue) from the best level to the worst; f returning
    // false stops the walk. Bids are best at the high end of the map.
    template <class F>
    static void walk(const Ladder& ladder, Side side, F&& f) {
        if (side == Side::Buy) {
            for (auto it = ladder.rbegin(); it != ladder.rend(); ++it) {
                if (!f(it->first, it->second)) {
                    return;
                }
            }
        } else {
            for (const auto& [price, queue] : ladder) {
                if (!f(price, queue)) {
                    return;
                }
            }
        }
    }

    OrderId reject(OwnerId owner, Token token, OrderId id, RejectReason reason, Nanos now) {
        ++stats_.rejected;
        reports_->on_rejected(Rejected{
            .owner = owner, .token = token, .order_id = id, .reason = reason, .timestamp = now});
        return 0;
    }

    // Cancels shares of an incoming order that found nothing to trade with.
    void drop(const Incoming& in, Qty shares, CancelReason reason, Nanos now) {
        stats_.unfilled_shares += shares;
        reports_->on_cancelled(Cancelled{.order_id = in.id,
                                         .owner = in.owner,
                                         .token = in.token,
                                         .qty = shares,
                                         .leaves = 0,
                                         .reason = reason,
                                         .timestamp = now});
    }

    // Whether a limit order on `side` at `limit` would trade right now.
    [[nodiscard]] bool would_trade(const Book& book, Side side, Price limit) const {
        const Side other = opposite(side);
        const Ladder& ladder = book.side(other);
        if (ladder.empty()) {
            return false;
        }
        const auto top = other == Side::Buy ? std::prev(ladder.end()) : ladder.begin();
        return acceptable(side, limit, top->first);
    }

    // Whether a fill-or-kill order would be filled completely: whether step 2
    // of submit, run on the book as it is, would trade `qty` shares.
    //
    // Hidden shares count. A replenished slice goes to the back of its level,
    // so the incoming order reaches it after everything else at that price.
    // That is still "at that price", except when one of the owner's own orders
    // is in the queue and prevention stops the incoming order there: then only
    // the displayed shares ahead of that order are reached.
    [[nodiscard]] bool can_fill(const Book& book, const Incoming& in, Price limit, Qty qty) const {
        const Side other = opposite(in.side);
        const bool stops_at_own =
            in.self_match == SelfMatch::CancelIncoming || in.self_match == SelfMatch::CancelBoth;
        const bool skips_own = in.self_match == SelfMatch::CancelResting;
        std::uint64_t available = 0;
        walk(book.side(other), other, [&](Price price, const Queue& queue) {
            if (!acceptable(in.side, limit, price)) {
                return false;
            }
            if (in.self_match == SelfMatch::Allow) {
                available += queue.total + queue.hidden;
                return available < qty;
            }
            const bool has_own =
                std::any_of(queue.orders.begin(), queue.orders.end(),
                            [&in](const Resting& order) { return order.owner == in.owner; });
            for (const Resting& order : queue.orders) {
                if (order.owner == in.owner) {
                    if (stops_at_own) {
                        return false;  // nothing beyond this order is reached
                    }
                    continue;  // skips_own: it will be cancelled, not traded with
                }
                available += order.qty;
                if (!has_own || skips_own) {
                    available += order.hidden;
                }
            }
            return available < qty;
        });
        return available >= qty;
    }

    // Trades an incoming order against the other side of the book until it is
    // used up or nothing acceptable is left. Returns the shares it has left.
    Qty match(Book& book, const Incoming& in, Price limit, Qty qty, Nanos now) {
        const Side other = opposite(in.side);
        Ladder& ladder = book.side(other);
        while (qty != 0 && !ladder.empty()) {
            const auto level = other == Side::Buy ? std::prev(ladder.end()) : ladder.begin();
            const Price price = level->first;
            if (!acceptable(in.side, limit, price)) {
                break;
            }
            Queue& queue = level->second;
            Resting& resting = queue.orders.front();

            if (in.self_match != SelfMatch::Allow && resting.owner == in.owner) {
                ++stats_.self_matches;
                if (in.self_match != SelfMatch::CancelIncoming) {
                    const Resting gone = resting;
                    index_.erase(gone.id);
                    queue.total -= gone.qty;
                    queue.hidden -= gone.hidden;
                    queue.orders.pop_front();
                    if (queue.orders.empty()) {
                        ladder.erase(level);
                    }
                    md_->on_delete(md::remove(in.locate, now, gone.ref));
                    reports_->on_cancelled(Cancelled{.order_id = gone.id,
                                                     .owner = gone.owner,
                                                     .token = gone.token,
                                                     .qty = gone.qty + gone.hidden,
                                                     .leaves = 0,
                                                     .reason = CancelReason::SelfMatch,
                                                     .timestamp = now});
                }
                if (in.self_match != SelfMatch::CancelResting) {
                    drop(in, qty, CancelReason::SelfMatch, now);
                    return 0;
                }
                continue;
            }

            const Qty fill = std::min(qty, resting.qty);
            const std::uint64_t match_number = ++last_match_;

            resting.qty -= fill;
            queue.total -= fill;
            qty -= fill;
            ++stats_.trades;
            stats_.traded_shares += fill;

            md_->on_execute(md::execute(in.locate, now, resting.ref, fill, match_number));
            reports_->on_executed(Executed{.order_id = resting.id,
                                           .owner = resting.owner,
                                           .token = resting.token,
                                           .qty = fill,
                                           .price = price,
                                           .leaves = resting.qty + resting.hidden,
                                           .match_number = match_number,
                                           .liquidity = Liquidity::Added,
                                           .timestamp = now});
            reports_->on_executed(Executed{.order_id = in.id,
                                           .owner = in.owner,
                                           .token = in.token,
                                           .qty = fill,
                                           .price = price,
                                           .leaves = qty,
                                           .match_number = match_number,
                                           .liquidity = Liquidity::Removed,
                                           .timestamp = now});

            if (resting.qty != 0) {
                continue;
            }
            if (resting.hidden != 0) {
                // An iceberg shows its next slice: under a new reference, and
                // from the back of the queue. splice moves the node without
                // invalidating the iterator the index holds.
                const Qty slice = std::min(resting.display, resting.hidden);
                resting.qty = slice;
                resting.hidden -= slice;
                resting.ref = ++last_id_;
                queue.total += slice;
                queue.hidden -= slice;
                const Resting& shown = resting;
                queue.orders.splice(queue.orders.end(), queue.orders, queue.orders.begin());
                md_->on_add(md::add(in.locate, now, shown.ref, other, slice, book.symbol, price));
            } else {
                index_.erase(resting.id);
                queue.orders.pop_front();
                if (queue.orders.empty()) {
                    ladder.erase(level);
                }
            }
        }
        return qty;
    }

    // Puts an order at the back of the queue at its price, showing as much of
    // it as its display size allows. Returns the shares shown.
    Qty rest(Book& book, const Incoming& in, Price price, Qty qty) {
        const Qty shown = in.display != 0 ? std::min(in.display, qty) : qty;
        Queue& queue = book.side(in.side)[price];
        queue.orders.push_back(Resting{.id = in.id,
                                       .owner = in.owner,
                                       .token = in.token,
                                       .qty = shown,
                                       .hidden = qty - shown,
                                       .display = in.display,
                                       .ref = in.id,
                                       .post_only = in.post_only,
                                       .self_match = in.self_match});
        queue.total += shown;
        queue.hidden += qty - shown;
        index_.emplace(in.id, Locator{in.locate, in.side, price, std::prev(queue.orders.end())});
        return shown;
    }

    // Takes a resting order out of the book and returns what it was.
    Resting unlink(typename Index::iterator found) {
        const Locator where = found->second;
        const Resting order = *where.at;
        Ladder& ladder = books_[where.locate].side(where.side);
        const auto level = ladder.find(where.price);
        level->second.total -= order.qty;
        level->second.hidden -= order.hidden;
        level->second.orders.erase(where.at);
        if (level->second.orders.empty()) {
            ladder.erase(level);
        }
        index_.erase(found);
        return order;
    }

    Reports* reports_;
    MarketData* md_;
    std::vector<Book> books_;
    Index index_;
    EngineStats stats_{};
    OrderId last_id_ = 0;
    std::uint64_t last_match_ = 0;
};

}  // namespace obe::engine
