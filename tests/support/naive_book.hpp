#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/feed/handler.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"

// An oracle for the book tests.
//
// NaiveBook keeps a flat list of orders and nothing else. It has no price
// levels and no incremental state: after each message it recomputes the best
// bid and offer of the affected security by scanning every order. That makes
// it far too slow for real data and very hard to get wrong, which is what an
// oracle should be. It shares no code with BookManager, so when the two agree
// on every update over a long random stream, the agreement means something.

namespace obe::test {

class NaiveBook : public feed::HandlerBase {
 public:
    std::vector<book::BboUpdate> updates;
    std::uint64_t errors = 0;  // any message that refers to an unknown order, and so on

    void on_add(const feed::AddOrder& m) {
        if (m.shares == 0 || !orders_.emplace(m.order_ref, order_from(m)).second) {
            ++errors;
            return;
        }
        after(m.hdr);
    }

    void on_execute(const feed::OrderExecuted& m) { reduce(m.hdr, m.order_ref, m.shares); }
    void on_execute_with_price(const feed::OrderExecutedWithPrice& m) {
        reduce(m.hdr, m.order_ref, m.shares);
    }
    void on_cancel(const feed::OrderCancel& m) { reduce(m.hdr, m.order_ref, m.shares); }

    void on_delete(const feed::OrderDelete& m) {
        if (orders_.erase(m.order_ref) == 0) {
            ++errors;
            return;
        }
        after(m.hdr);
    }

    void on_replace(const feed::OrderReplace& m) {
        const auto it = orders_.find(m.orig_order_ref);
        if (it == orders_.end() || m.shares == 0 || orders_.contains(m.new_order_ref)) {
            ++errors;
            return;
        }
        Order next = it->second;
        next.price = m.price;
        next.qty = m.shares;
        orders_.erase(it);
        orders_.emplace(m.new_order_ref, next);
        after(m.hdr);
    }

    // The book of one security, recomputed from scratch.
    [[nodiscard]] std::vector<book::Level> levels(Locate locate, Side side) const {
        std::map<Price, std::uint64_t> by_price;
        for (const auto& [ref, order] : orders_) {
            if (order.locate == locate && order.side == side) {
                by_price[order.price] += order.qty;
            }
        }
        // Best to worst: ascending for asks, descending for bids.
        std::vector<book::Level> out;
        out.reserve(by_price.size());
        for (const auto& [price, qty] : by_price) {
            out.push_back({price, qty});
        }
        if (side == Side::Buy) {
            std::reverse(out.begin(), out.end());
        }
        return out;
    }

    [[nodiscard]] std::size_t open_orders() const { return orders_.size(); }

 private:
    struct Order {
        Locate locate;
        Side side;
        Price price;
        Qty qty;
    };

    static Order order_from(const feed::AddOrder& m) {
        return {m.hdr.locate, m.side, m.price, m.shares};
    }

    void reduce(const feed::Header& hdr, OrderId ref, Qty shares) {
        const auto it = orders_.find(ref);
        if (it == orders_.end() || shares == 0 || shares > it->second.qty) {
            ++errors;
            return;
        }
        it->second.qty -= shares;
        if (it->second.qty == 0) {
            orders_.erase(it);
        }
        after(hdr);
    }

    [[nodiscard]] book::Bbo scan(Locate locate) const {
        book::Bbo bbo;
        for (const auto& [ref, order] : orders_) {
            if (order.locate != locate) {
                continue;
            }
            if (order.side == Side::Buy) {
                if (bbo.bid_qty == 0 || order.price > bbo.bid_price) {
                    bbo.bid_price = order.price;
                    bbo.bid_qty = order.qty;
                } else if (order.price == bbo.bid_price) {
                    bbo.bid_qty += order.qty;
                }
            } else {
                if (bbo.ask_qty == 0 || order.price < bbo.ask_price) {
                    bbo.ask_price = order.price;
                    bbo.ask_qty = order.qty;
                } else if (order.price == bbo.ask_price) {
                    bbo.ask_qty += order.qty;
                }
            }
        }
        return bbo;
    }

    void after(const feed::Header& hdr) {
        const book::Bbo now = scan(hdr.locate);
        book::Bbo& last = last_[hdr.locate];
        if (now != last) {
            last = now;
            updates.push_back({hdr.locate, hdr.timestamp, now});
        }
    }

    std::map<OrderId, Order> orders_;
    std::map<Locate, book::Bbo> last_;
};

}  // namespace obe::test
