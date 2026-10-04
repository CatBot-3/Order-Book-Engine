#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "obe/book/book.hpp"
#include "obe/book/concepts.hpp"
#include "obe/book/types.hpp"
#include "obe/feed/handler.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"

// Rebuilds the displayed book of every security from an ITCH feed.
//
// BookManager is an ITCH handler: give it to ItchParser and it applies the
// seven book-changing messages. Every other message type falls through to the
// empty defaults in HandlerBase, which is what guarantees that 'P' and 'Q'
// prints cannot move the book.
//
// It is a template over the two containers, so the same logic drives the
// reference implementation and every optimized one, and over the listener, so
// publishing a best-bid-and-offer change is a direct call.
//
// Layout:
//   one order store for the whole feed   reference number -> price, side, shares
//   one Book per stock locate            a flat array indexed by locate
//
// The array has an entry for every possible 16-bit locate. That costs memory
// up front and removes both a hash lookup and a bounds check from every
// message. A real day uses under ten thousand of the 65536.

namespace obe::book {

template <OrderStoreLike Store, PriceLevelsLike Levels, BboListener Listener = NullBboListener>
class BookManager : public feed::HandlerBase {
 public:
    using BookType = Book<Levels>;

    static constexpr std::size_t kLocates = std::size_t{1} << 16;

    BookManager() : BookManager(Store{}) {}

    explicit BookManager(Store store, Listener listener = Listener{})
        : store_(std::move(store)),
          listener_(std::move(listener)),
          books_(kLocates),
          symbols_(kLocates),
          listed_(kLocates, false) {}

    // --- ITCH callbacks ------------------------------------------------------

    void on_stock_directory(const feed::StockDirectory& m) {
        symbols_[m.hdr.locate] = m.stock;
        listed_[m.hdr.locate] = true;
    }

    void on_trading_action(const feed::TradingAction& m) {
        books_[m.hdr.locate].trading_state_ = m.trading_state;
    }

    // 'A' and 'F'. The attribution on 'F' does not affect the book.
    void on_add(const feed::AddOrder& m) {
        if (m.shares == 0) [[unlikely]] {
            ++counters_.zero_shares;
            return;
        }
        if (!store_.insert(m.order_ref, OrderRecord{m.price, m.shares, m.side})) [[unlikely]] {
            ++counters_.duplicate_order;
            return;
        }
        BookType& book = books_[m.hdr.locate];
        book.side(m.side).add(m.price, m.shares);
        open_shares_ += m.shares;
        publish(book, m.hdr);
    }

    // 'E'. Always counts towards volume.
    void on_execute(const feed::OrderExecuted& m) { reduce(m.hdr, m.order_ref, m.shares, true); }

    // 'C'. The execution price can differ from the order's display price, but
    // the shares still come off the level the order is displayed at. Only
    // printable executions count towards volume: the non-printable ones are
    // reported again in bulk by a later cross message.
    void on_execute_with_price(const feed::OrderExecutedWithPrice& m) {
        reduce(m.hdr, m.order_ref, m.shares, m.is_printable());
    }

    // 'X'. A partial cancel; `shares` is the amount removed.
    void on_cancel(const feed::OrderCancel& m) { reduce(m.hdr, m.order_ref, m.shares, false); }

    // 'D'. Removes whatever is left of the order.
    void on_delete(const feed::OrderDelete& m) {
        const OrderRecord* rec = store_.find(m.order_ref);
        if (rec == nullptr) [[unlikely]] {
            ++counters_.unknown_order;
            return;
        }
        BookType& book = books_[m.hdr.locate];
        take_from_level(book, *rec, rec->qty);
        store_.erase(m.order_ref);
        publish(book, m.hdr);
    }

    // 'U'. The original order goes; a new one under a new reference number
    // takes its place on the same side. The side is not in the message, so it
    // has to be read from the original before that is erased.
    void on_replace(const feed::OrderReplace& m) {
        const OrderRecord* found = store_.find(m.orig_order_ref);
        if (found == nullptr) [[unlikely]] {
            ++counters_.unknown_order;
            return;
        }
        // Copy: the pointer does not survive the erase and insert below.
        const OrderRecord old = *found;
        BookType& book = books_[m.hdr.locate];
        take_from_level(book, old, old.qty);
        store_.erase(m.orig_order_ref);

        if (m.shares == 0) [[unlikely]] {
            ++counters_.zero_shares;
        } else if (!store_.insert(m.new_order_ref, OrderRecord{m.price, m.shares, old.side}))
            [[unlikely]] {
            ++counters_.duplicate_order;
        } else {
            book.side(old.side).add(m.price, m.shares);
            open_shares_ += m.shares;
        }
        // One publish for the whole replace: the moment between the removal
        // and the re-add is not a state the exchange ever displayed.
        publish(book, m.hdr);
    }

    // --- Hints ---------------------------------------------------------------

    // Tells the order store that `id` is about to be looked up, so it can
    // start pulling the relevant memory into cache. A no-op for stores that do
    // not offer prefetch(). The replay loop calls this one message ahead: the
    // next message is already in the buffer, so its lookup can begin while the
    // current message is still being handled (phase 4, experiment 6).
    void prefetch(OrderId id) const noexcept {
        if constexpr (requires(const Store& s) { s.prefetch(id); }) {
            store_.prefetch(id);
        } else {
            static_cast<void>(id);
        }
    }

    // --- Queries -------------------------------------------------------------

    [[nodiscard]] const BookType& book(Locate locate) const noexcept { return books_[locate]; }
    [[nodiscard]] const Store& orders() const noexcept { return store_; }
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] Listener& listener() noexcept { return listener_; }
    [[nodiscard]] const Listener& listener() const noexcept { return listener_; }

    // True once a Stock Directory message has been seen for this locate.
    [[nodiscard]] bool listed(Locate locate) const noexcept { return listed_[locate]; }
    [[nodiscard]] std::string_view symbol(Locate locate) const noexcept {
        return symbols_[locate].view();
    }

    // The locate for a symbol, from this day's Stock Directory messages. A
    // linear scan: it is meant for start-up and tools, not for the hot path.
    [[nodiscard]] std::optional<Locate> find_locate(std::string_view symbol_name) const noexcept {
        for (std::size_t i = 0; i < kLocates; ++i) {
            if (listed_[i] && symbols_[i].view() == symbol_name) {
                return static_cast<Locate>(i);
            }
        }
        return std::nullopt;
    }

    // Walks every level of every book. Slow; for end-of-run checks and tests.
    [[nodiscard]] Audit audit() const {
        Audit out;
        const auto visit = [&out](const Level& level) {
            ++out.levels;
            out.level_shares += level.qty;
            if (level.qty == 0) {
                ++out.empty_levels;
            }
            return true;
        };
        for (const BookType& b : books_) {
            b.bids().for_each(visit);
            b.asks().for_each(visit);
        }
        out.open_shares = open_shares_;
        out.open_orders = store_.size();
        return out;
    }

 private:
    // Shared by 'E', 'C' and 'X': take `shares` off a resting order.
    void reduce(const feed::Header& hdr, OrderId ref, Qty shares, bool counts_as_volume) {
        if (shares == 0) [[unlikely]] {
            ++counters_.zero_shares;
            return;
        }
        OrderRecord* rec = store_.find(ref);
        if (rec == nullptr) [[unlikely]] {
            ++counters_.unknown_order;
            return;
        }
        if (shares > rec->qty) [[unlikely]] {
            // The remaining quantity would go negative. Record it and take
            // only what is there, so one bad message cannot wrap an unsigned
            // quantity into billions of phantom shares.
            ++counters_.overfill;
            shares = rec->qty;
        }
        BookType& book = books_[hdr.locate];
        take_from_level(book, *rec, shares);
        if (counts_as_volume) {
            book.executed_shares_ += shares;
        }
        rec->qty -= shares;
        if (rec->qty == 0) {
            // ITCH sends no delete for an order that was filled completely.
            store_.erase(ref);
        }
        publish(book, hdr);
    }

    // `shares` are leaving the order `rec`; take them off its level too.
    //
    // open_shares_ follows the orders, not the levels: it drops whether or not
    // the level agreed. If the two containers ever disagree, the levels keep
    // shares that no order accounts for, and audit() sees the gap.
    void take_from_level(BookType& book, const OrderRecord& rec, Qty shares) {
        open_shares_ -= shares;
        if (!book.side(rec.side).remove(rec.price, shares)) [[unlikely]] {
            ++counters_.level_mismatch;
        }
    }

    // Tell the listener if the best bid or offer is not what it was last told.
    void publish(BookType& book, const feed::Header& hdr) {
        const Bbo now = book.bbo();
        if (now == book.last_published_) {
            return;
        }
        book.last_published_ = now;
        ++stats_.bbo_updates;
        if (now.has_bid() && now.has_ask() && now.bid_price >= now.ask_price) [[unlikely]] {
            const bool trading = book.trading_state_ == 'T';
            if (now.bid_price == now.ask_price) {
                ++(trading ? stats_.locked_while_trading : stats_.locked_while_not_trading);
            } else {
                ++(trading ? stats_.crossed_while_trading : stats_.crossed_while_not_trading);
            }
        }
        listener_.on_bbo(BboUpdate{hdr.locate, hdr.timestamp, now});
    }

    Store store_;
    Listener listener_;
    std::vector<BookType> books_;
    std::vector<feed::Symbol> symbols_;
    std::vector<bool> listed_;
    Counters counters_{};
    Stats stats_{};
    std::uint64_t open_shares_ = 0;
};

}  // namespace obe::book
