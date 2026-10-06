#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/concepts.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/book/types.hpp"
#include "obe/feed/handler.hpp"
#include "obe/feed/messages.hpp"
#include "obe/sim/strategy.hpp"
#include "obe/sim/types.hpp"
#include "obe/types.hpp"

// Replays a recorded feed and works out what would have happened to a
// strategy's quotes in one security.
//
// MarketMakingSim is an ITCH handler, like BookManager, and contains one: every
// message goes on to the real book unchanged. For the quoted security it looks
// at each message first, while the order the message names is still as it was.
//
// THE FILL MODEL, AND WHAT IT ASSUMES
//
// A simulated quote rests at a price, behind everything that was displayed at
// that price when it joined. ITCH names the order in every execution, cancel
// and delete, so for each real order at our price it is known whether it was
// there before us (ahead) or came later (behind). That gives three ways for a
// quote to trade, all of which follow from price-time priority:
//
//   1. A real order BEHIND us at our price is executed. Whoever traded with it
//      would have traded with us first.
//   2. A real order at a WORSE price than ours is executed. Same reasoning:
//      the trade went past our price to reach it.
//   3. Our quote is better than every real one on its side, and a real order
//      arrives on the other side at a price that reaches ours. In the recorded
//      market it rested; with our quote there it would have traded.
//
// and one way for it to move up the queue: a real order AHEAD of us is
// executed, cancelled or deleted.
//
// The assumptions behind that, each of which flatters the result or hurts it:
//
//   a. NO MARKET IMPACT. The recorded orders arrive exactly as they did. In
//      reality a displayed quote changes what others do; and the shares that
//      "would have traded with us first" still trade with the recorded order
//      as well, so that liquidity is counted twice. Flatters.
//   b. NO HIDDEN ORDERS. Executions against non-displayed orders ('P'
//      messages) are ignored: they do not fill us and do not move the queue.
//      A hidden order at a better price would have been ahead of us. Mixed.
//   c. QUEUE POSITION IS EXACT for displayed orders, because the feed is
//      order by order. A feed that only gives level totals cannot tell a
//      cancel ahead of us from one behind, and has to assume. This one does
//      not have to.
//   d. LATENCY is one fixed number (SimConfig::latency), applied to new
//      quotes and to cancels alike. Real latency varies, and is worst exactly
//      when the market is busiest.
//   e. ONE CHANGE IN FLIGHT per side. While a cancel or a new quote is on its
//      way, further wishes for that side wait until it has landed.
//   f. QUOTES ARE PASSIVE. One that would trade on arrival is refused, never
//      executed as a taker.
//
// It follows that this is a way of comparing strategies under the same stated
// assumptions, and of seeing adverse selection in the markouts. It is not an
// estimate of what a strategy would earn.

namespace obe::sim {

// What is resting for us on one side.
struct RestingQuote {
    Price price = 0;
    Qty qty = 0;              // shares not yet filled
    std::uint64_t ahead = 0;  // displayed shares that must go before we trade at this price

    friend bool operator==(const RestingQuote&, const RestingQuote&) = default;
};

template <Strategy S, book::OrderStoreLike Store = book::OrderStore,
          book::PriceLevelsLike Levels = book::PriceLevels>
class MarketMakingSim : public feed::HandlerBase {
 public:
    using Manager = book::BookManager<Store, Levels>;

    MarketMakingSim(const SimConfig& cfg, S& strategy, Store store = Store{})
        : cfg_(cfg), strategy_(&strategy), book_(std::move(store)) {
        for (std::size_t i = 0; i < markouts_.size(); ++i) {
            markouts_[i].stats.horizon = cfg_.markout_horizons[i];
        }
    }

    // --- ITCH callbacks ------------------------------------------------------

    void on_stock_directory(const feed::StockDirectory& m) {
        book_.on_stock_directory(m);
        if (locate_ < 0 && m.stock.view() == cfg_.symbol) {
            locate_ = static_cast<int>(m.hdr.locate);
        }
    }

    void on_trading_action(const feed::TradingAction& m) {
        if (!ours(m.hdr)) {
            book_.on_trading_action(m);
            return;
        }
        advance(m.hdr.timestamp);
        book_.on_trading_action(m);
        dirty_ = true;  // a halt pulls the quotes; a resumption lets them back
        after(m.hdr.timestamp);
    }

    void on_add(const feed::AddOrder& m) {
        if (!ours(m.hdr)) {
            book_.on_add(m);
            return;
        }
        advance(m.hdr.timestamp);
        // The book refuses an order with no shares or a reference number that
        // is already live. So must we, or the two would disagree about what
        // is in the queue.
        if (m.shares != 0 && book_.orders().find(m.order_ref) == nullptr) {
            real_added(m.order_ref, m.side, m.price, m.shares, m.hdr.timestamp);
        }
        book_.on_add(m);
        after(m.hdr.timestamp);
    }

    void on_execute(const feed::OrderExecuted& m) {
        if (!ours(m.hdr)) {
            book_.on_execute(m);
            return;
        }
        advance(m.hdr.timestamp);
        real_reduced(m.order_ref, m.shares, Reduction::Executed, true, m.hdr.timestamp);
        book_.on_execute(m);
        after(m.hdr.timestamp);
    }

    // The execution price of a 'C' can differ from the order's displayed
    // price. Priority is decided by the displayed price, so that is what the
    // fill model uses, exactly as the book does.
    void on_execute_with_price(const feed::OrderExecutedWithPrice& m) {
        if (!ours(m.hdr)) {
            book_.on_execute_with_price(m);
            return;
        }
        advance(m.hdr.timestamp);
        real_reduced(m.order_ref, m.shares, Reduction::Executed, m.is_printable(), m.hdr.timestamp);
        book_.on_execute_with_price(m);
        after(m.hdr.timestamp);
    }

    void on_cancel(const feed::OrderCancel& m) {
        if (!ours(m.hdr)) {
            book_.on_cancel(m);
            return;
        }
        advance(m.hdr.timestamp);
        real_reduced(m.order_ref, m.shares, Reduction::Withdrawn, false, m.hdr.timestamp);
        book_.on_cancel(m);
        after(m.hdr.timestamp);
    }

    void on_delete(const feed::OrderDelete& m) {
        if (!ours(m.hdr)) {
            book_.on_delete(m);
            return;
        }
        advance(m.hdr.timestamp);
        real_removed(m.order_ref, m.hdr.timestamp);
        book_.on_delete(m);
        after(m.hdr.timestamp);
    }

    // A replace is the old order leaving and a new one arriving at the back of
    // its new level, which is how the exchange treats it too.
    void on_replace(const feed::OrderReplace& m) {
        if (!ours(m.hdr)) {
            book_.on_replace(m);
            return;
        }
        advance(m.hdr.timestamp);
        if (const book::OrderRecord* old = book_.orders().find(m.orig_order_ref)) {
            const Side side = old->side;
            real_removed(m.orig_order_ref, m.hdr.timestamp);
            // The book adds the new order unless it has no shares or its
            // number is live (the old one, about to go, does not count).
            const bool number_free = m.new_order_ref == m.orig_order_ref ||
                                     book_.orders().find(m.new_order_ref) == nullptr;
            if (m.shares != 0 && number_free) {
                real_added(m.new_order_ref, side, m.price, m.shares, m.hdr.timestamp);
            }
        }
        book_.on_replace(m);
        after(m.hdr.timestamp);
    }

    // --- After the replay ----------------------------------------------------

    // Call once, when the feed has ended. Fills too recent for their markout
    // to have been measured are counted as such.
    void finish() {
        for (MarkoutQueue& q : markouts_) {
            for (const PendingMarkout& p : q.pending) {
                q.stats.unmeasured_shares += p.qty;
            }
            q.pending.clear();
        }
    }

    [[nodiscard]] SimReport report() const {
        SimReport r = counts_;
        r.inventory = inventory_;
        r.cash = cash_;
        r.mid2 = mid2_;
        r.pnl2 = pnl2();
        r.max_drawdown2 = max_drawdown2_;
        r.mean_abs_inventory = window_time_ > 0 ? abs_inventory_time_ / window_time_ : 0.0;
        for (std::size_t i = 0; i < markouts_.size(); ++i) {
            r.markouts[i] = markouts_[i].stats;
        }
        return r;
    }

    // --- Queries -------------------------------------------------------------

    [[nodiscard]] const Manager& book() const noexcept { return book_; }
    [[nodiscard]] const std::vector<Fill>& fills() const noexcept { return fills_; }
    [[nodiscard]] const SimConfig& config() const noexcept { return cfg_; }

    // The locate of the quoted security, once its Stock Directory message has
    // been seen.
    [[nodiscard]] std::optional<Locate> locate() const noexcept {
        return locate_ < 0 ? std::nullopt : std::optional<Locate>(static_cast<Locate>(locate_));
    }

    // What is resting for us on a side, if anything.
    [[nodiscard]] std::optional<RestingQuote> working(Side side) const noexcept {
        const std::optional<Working>& w = working_[index(side)];
        if (!w) {
            return std::nullopt;
        }
        return RestingQuote{w->price, w->qty, w->ahead};
    }

    // True while a cancel or a new quote for this side is on its way.
    [[nodiscard]] bool in_flight(Side side) const noexcept {
        return change_[index(side)].has_value();
    }

 private:
    enum class Reduction : std::uint8_t { Executed, Withdrawn };

    struct Working {
        Price price = 0;
        Qty qty = 0;
        std::uint64_t ahead = 0;
        // Real orders that arrived up to this stamp are ahead of us; later
        // ones are behind.
        std::uint64_t stamp = 0;
    };

    // A cancel, a new quote, or both, that takes effect at `at`.
    struct Change {
        Nanos at = 0;
        bool cancel = false;
        Quote place;
    };

    struct PendingMarkout {
        Nanos due = 0;
        Side side = Side::Buy;
        Price price = 0;
        Qty qty = 0;
    };
    struct MarkoutQueue {
        Markout stats;
        std::deque<PendingMarkout> pending;  // in order of `due`: the horizon is fixed
    };

    [[nodiscard]] static constexpr std::size_t index(Side side) noexcept {
        return side == Side::Buy ? 0 : 1;
    }
    [[nodiscard]] static constexpr Side opposite(Side side) noexcept {
        return side == Side::Buy ? Side::Sell : Side::Buy;
    }
    // Is price `a` further from the market than `b`, for a resting order on
    // this side? A lower bid, a higher ask.
    [[nodiscard]] static constexpr bool worse(Side side, Price a, Price b) noexcept {
        return side == Side::Buy ? a < b : a > b;
    }

    [[nodiscard]] bool ours(const feed::Header& hdr) const noexcept {
        return static_cast<int>(hdr.locate) == locate_;
    }

    // May a quote rest at this moment?
    [[nodiscard]] bool open(Nanos now) const noexcept {
        return now >= cfg_.start && now < cfg_.end &&
               book_.book(static_cast<Locate>(locate_)).trading_state() == 'T';
    }

    [[nodiscard]] std::int64_t pnl2() const noexcept { return 2 * cash_ + inventory_ * mid2_; }

    // --- Time ----------------------------------------------------------------

    // Brings the simulation up to `now`, the timestamp of a message about to
    // be applied. Everything here sees the book as it was before that message,
    // which is the book as it stood at any moment since the previous message
    // of this security.
    void advance(Nanos now) {
        if (started_) {
            const Nanos from = std::max(last_time_, cfg_.start);
            const Nanos to = std::min(now, cfg_.end);
            if (to > from) {
                const auto dt = static_cast<double>(to - from);
                const auto held = static_cast<double>(inventory_ < 0 ? -inventory_ : inventory_);
                abs_inventory_time_ += held * dt;
                window_time_ += dt;
            }
        }
        started_ = true;
        last_time_ = now;

        // The end of the window is known in advance, so quotes are out by
        // then: no latency applies, and nothing still in flight lands.
        if (now >= cfg_.end) {
            for (std::size_t s = 0; s < 2; ++s) {
                if (working_[s]) {
                    working_[s].reset();
                    ++counts_.cancelled;
                }
                if (change_[s]) {
                    if (change_[s]->place.live()) {
                        ++counts_.rejected_closed;
                    }
                    change_[s].reset();
                }
            }
        }
        if (land_due(change_, now)) {
            dirty_ = true;  // the strategy should see what its changes came to
        }
        for (MarkoutQueue& q : markouts_) {
            while (!q.pending.empty() && q.pending.front().due <= now) {
                const PendingMarkout& p = q.pending.front();
                q.stats.shares += p.qty;
                q.stats.sum2 += static_cast<std::int64_t>(p.qty) * edge2(p.side, p.price);
                q.pending.pop_front();
            }
        }
    }

    // After the message has been applied to the book.
    void after(Nanos now) {
        const book::Bbo bbo = book_.book(static_cast<Locate>(locate_)).bbo();
        if (bbo != bbo_) {
            bbo_ = bbo;
            dirty_ = true;
            if (bbo.has_bid() && bbo.has_ask()) {
                const std::int64_t mid2 = static_cast<std::int64_t>(bbo.bid_price) +
                                          static_cast<std::int64_t>(bbo.ask_price);
                // Before the first mid there is no position to revalue: quotes
                // are only placed in a two-sided market.
                counts_.inventory_pnl2 += inventory_ * (mid2 - mid2_);
                mid2_ = mid2;
                note_pnl();
            }
        }
        if (dirty_) {
            dirty_ = false;
            decide(now);
        }
    }

    // --- The strategy's wishes -----------------------------------------------

    void decide(Nanos now) {
        MarketView view;
        view.now = now;
        view.bbo = bbo_;
        view.inventory = inventory_;
        view.tick = cfg_.tick;
        view.end = cfg_.end;
        if (working_[0]) {
            view.working_bid = {working_[0]->price, working_[0]->qty};
        }
        if (working_[1]) {
            view.working_ask = {working_[1]->price, working_[1]->qty};
        }

        // The strategy is asked only while the real market is open and has a
        // bid below an offer. Otherwise it wants nothing, and what is resting
        // is withdrawn: with one side missing, or the book locked or crossed,
        // there is no mid to quote around.
        Quotes want;
        if (open(now) && view.two_sided()) {
            ++counts_.decisions;
            want = strategy_->quote(view);
            if (want.bid.live() && want.ask.live() && want.bid.price >= want.ask.price) {
                ++counts_.rejected_self_cross;
                want = {};
            }
        }
        std::array<std::optional<Change>, 2> changes{needed(Side::Buy, want.bid, now),
                                                     needed(Side::Sell, want.ask, now)};
        if (cfg_.latency == 0) {
            land_due(changes, now);
            return;
        }
        for (std::size_t s = 0; s < 2; ++s) {
            if (changes[s]) {
                change_[s] = changes[s];
            }
        }
    }

    // What has to happen on one side to get from what is resting to what is
    // wanted, if anything.
    [[nodiscard]] std::optional<Change> needed(Side side, const Quote& want, Nanos now) const {
        const std::size_t s = index(side);
        if (change_[s]) {
            return std::nullopt;  // assumption (e): wait for the change in flight
        }
        const bool resting = working_[s].has_value();
        if (resting && want.live() && want.price == working_[s]->price) {
            return std::nullopt;  // same price: keep the place in the queue
        }
        if (!resting && !want.live()) {
            return std::nullopt;
        }
        return Change{now + cfg_.latency, resting, want};
    }

    // Carries out the changes whose time has come, and clears them. Returns
    // whether there were any.
    //
    // In the order they would reach the exchange: by time, and at the same
    // time cancels before new quotes. The second rule is what lets a strategy
    // move both quotes up together: the new bid may go where the old ask was,
    // because by the time it arrives the old ask has gone.
    bool land_due(std::array<std::optional<Change>, 2>& changes, Nanos now) {
        const std::array<bool, 2> due{changes[0] && changes[0]->at <= now,
                                      changes[1] && changes[1]->at <= now};
        if (due[0] && due[1] && changes[0]->at == changes[1]->at) {
            withdraw(0, *changes[0]);
            withdraw(1, *changes[1]);
            rest(0, *changes[0], now);
            rest(1, *changes[1], now);
        } else {
            // At most one, or two at different times: the earlier one whole,
            // then the later one.
            const std::size_t first = due[0] && due[1] && changes[1]->at < changes[0]->at ? 1 : 0;
            for (const std::size_t s : {first, 1 - first}) {
                if (due[s]) {
                    withdraw(s, *changes[s]);
                    rest(s, *changes[s], now);
                }
            }
        }
        for (std::size_t s = 0; s < 2; ++s) {
            if (due[s]) {
                changes[s].reset();
            }
        }
        return due[0] || due[1];
    }

    // The two halves of a change.
    void withdraw(std::size_t s, const Change& change) {
        if (change.cancel && working_[s]) {
            working_[s].reset();
            ++counts_.cancelled;
        }
    }
    void rest(std::size_t s, const Change& change, Nanos now) {
        if (change.place.live()) {
            place(s == 0 ? Side::Buy : Side::Sell, change.place, now);
        }
    }

    void place(Side side, const Quote& quote, Nanos now) {
        if (!open(now)) {
            ++counts_.rejected_closed;
            return;
        }
        if (cfg_.tick != 0 && quote.price % cfg_.tick != 0) {
            ++counts_.rejected_off_tick;
            return;
        }
        // Assumption (f): a quote that reaches the other side of the real
        // book would trade at once, and is refused.
        const bool crosses = side == Side::Buy ? (bbo_.has_ask() && quote.price >= bbo_.ask_price)
                                               : (bbo_.has_bid() && quote.price <= bbo_.bid_price);
        if (crosses) {
            ++counts_.rejected_crossing;
            return;
        }
        // Nor may it reach our own quote on the other side.
        const std::optional<Working>& other = working_[index(opposite(side))];
        if (other &&
            (side == Side::Buy ? quote.price >= other->price : quote.price <= other->price)) {
            ++counts_.rejected_self_cross;
            return;
        }
        working_[index(side)] =
            Working{quote.price, quote.qty, displayed_at(side, quote.price), stamp_};
        ++counts_.placed;
    }

    // Displayed shares resting at one price on one side of the real book.
    [[nodiscard]] std::uint64_t displayed_at(Side side, Price price) const {
        std::uint64_t qty = 0;
        book_.book(static_cast<Locate>(locate_)).side(side).for_each([&](const book::Level& level) {
            if (level.price == price) {
                qty = level.qty;
                return false;
            }
            return !worse(side, level.price, price);  // stop once past it
        });
        return qty;
    }

    // --- What the real orders do to our quotes --------------------------------

    // A real order is about to join the book.
    void real_added(OrderId ref, Side side, Price price, Qty shares, Nanos now) {
        arrival_[ref] = ++stamp_;

        // Way 3: would it have met our quote on the other side?
        const Side our_side = opposite(side);
        std::optional<Working>& w = working_[index(our_side)];
        if (!w) {
            return;
        }
        const bool reaches = our_side == Side::Buy ? price <= w->price : price >= w->price;
        // Only if ours is the best price on its side. If a real order is at
        // least as good, the arrival would have met that one first, and in the
        // recorded market it did not trade, so the book is locked or crossed
        // and nothing can be concluded.
        const bool best = our_side == Side::Buy ? (!bbo_.has_bid() || w->price > bbo_.bid_price)
                                                : (!bbo_.has_ask() || w->price < bbo_.ask_price);
        if (reaches && best) {
            fill(our_side, std::min(w->qty, shares), FillReason::CrossedByAdd, now);
        }
    }

    // `shares` are about to leave a real order.
    void real_reduced(OrderId ref, Qty shares, Reduction how, bool counts_as_volume, Nanos now) {
        const book::OrderRecord* rec = book_.orders().find(ref);
        if (rec == nullptr || shares == 0) {
            return;  // the book will count it as an error and change nothing
        }
        const Qty taken = std::min(shares, rec->qty);
        const auto arrived = arrival_.find(ref);
        const std::uint64_t stamp = arrived == arrival_.end() ? 0 : arrived->second;
        const bool executed = how == Reduction::Executed;
        if (executed && counts_as_volume && open(now)) {
            counts_.market_executed += taken;
        }

        const Side side = rec->side;
        const Price price = rec->price;
        const bool gone = taken == rec->qty;
        if (std::optional<Working>& w = working_[index(side)]) {
            if (price == w->price) {
                if (stamp <= w->stamp) {
                    // It was ahead of us: the queue in front gets shorter.
                    w->ahead -= std::min<std::uint64_t>(w->ahead, taken);
                } else if (executed) {
                    // Way 1: it was behind us, and it traded.
                    fill(side, std::min(w->qty, taken), FillReason::QueueReached, now);
                }
            } else if (executed && worse(side, price, w->price)) {
                // Way 2: the trade went past our price.
                fill(side, std::min(w->qty, taken), FillReason::TradedThrough, now);
            }
        }
        if (gone && arrived != arrival_.end()) {
            arrival_.erase(arrived);
        }
    }

    // A real order is about to be removed whole, without trading.
    void real_removed(OrderId ref, Nanos now) {
        if (const book::OrderRecord* rec = book_.orders().find(ref)) {
            real_reduced(ref, rec->qty, Reduction::Withdrawn, false, now);
        }
    }

    // --- Money ---------------------------------------------------------------

    // Twice the distance of a trade at `price` from the current mid, signed so
    // that positive is in our favour.
    [[nodiscard]] std::int64_t edge2(Side side, Price price) const noexcept {
        const std::int64_t price2 = 2 * static_cast<std::int64_t>(price);
        return side == Side::Buy ? mid2_ - price2 : price2 - mid2_;
    }

    void fill(Side side, Qty qty, FillReason reason, Nanos now) {
        if (qty == 0) {
            return;
        }
        Working& w = *working_[index(side)];
        const Price price = w.price;
        const auto shares = static_cast<std::int64_t>(qty);
        const std::int64_t value = static_cast<std::int64_t>(price) * shares;
        if (side == Side::Buy) {
            cash_ -= value;
            inventory_ += shares;
            counts_.bought += qty;
        } else {
            cash_ += value;
            inventory_ -= shares;
            counts_.sold += qty;
        }
        cash_ += cfg_.rebate * shares;
        counts_.rebates += cfg_.rebate * shares;
        counts_.spread2 += shares * edge2(side, price);
        counts_.max_long = std::max(counts_.max_long, inventory_);
        counts_.max_short = std::max(counts_.max_short, -inventory_);

        ++counts_.fills;
        switch (reason) {
            case FillReason::QueueReached:
                ++counts_.fills_queue;
                break;
            case FillReason::TradedThrough:
                ++counts_.fills_through;
                break;
            case FillReason::CrossedByAdd:
                ++counts_.fills_crossed;
                break;
        }
        fills_.push_back(Fill{now, side, price, qty, mid2_, reason});
        for (MarkoutQueue& q : markouts_) {
            q.pending.push_back(PendingMarkout{now + q.stats.horizon, side, price, qty});
        }

        w.qty -= qty;
        if (w.qty == 0) {
            working_[index(side)].reset();
        }
        note_pnl();
        dirty_ = true;
    }

    void note_pnl() noexcept {
        const std::int64_t now = pnl2();
        peak_pnl2_ = std::max(peak_pnl2_, now);
        max_drawdown2_ = std::max(max_drawdown2_, peak_pnl2_ - now);
    }

    SimConfig cfg_;
    S* strategy_;
    Manager book_;
    int locate_ = -1;  // of the quoted security; -1 until its directory message

    // Arrival order of the real orders in the quoted security.
    std::unordered_map<OrderId, std::uint64_t> arrival_;
    std::uint64_t stamp_ = 0;

    std::array<std::optional<Working>, 2> working_{};  // bid, ask
    std::array<std::optional<Change>, 2> change_{};
    book::Bbo bbo_{};  // the real best bid and offer after the last message
    bool dirty_ = false;

    std::int64_t inventory_ = 0;
    std::int64_t cash_ = 0;
    std::int64_t mid2_ = 0;
    std::int64_t peak_pnl2_ = 0;
    std::int64_t max_drawdown2_ = 0;
    SimReport counts_{};
    std::vector<Fill> fills_;
    std::array<MarkoutQueue, 2> markouts_{};

    bool started_ = false;
    Nanos last_time_ = 0;
    double abs_inventory_time_ = 0;
    double window_time_ = 0;
};

template <Strategy S>
MarketMakingSim(const SimConfig&, S&) -> MarketMakingSim<S>;

}  // namespace obe::sim
