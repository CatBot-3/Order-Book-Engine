#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/messages.hpp"
#include "obe/types.hpp"
#include "obe/util/pool.hpp"

// MatchingEngine: the pooled matching engine of phase 5. It does what
// ReferenceEngine does (the contract is in concepts.hpp), built the way the
// spec asks: per side, price levels that each hold an intrusive
// first-in-first-out list of orders taken from a pool, and a lookup from order
// id to the order.
//
// The judges:
//
//   tests/engine/engine_scenario_test.cpp      every order type and edge case
//   tests/engine/instructions_test.cpp         post-only, icebergs, self-match prevention
//   tests/engine/engine_property_test.cpp      invariants over seeded random flow
//   tests/engine/round_trip_test.cpp           criterion 7: its feed rebuilds its book
//   tests/engine/engine_differential_test.cpp  identical output to the reference
//   tests/journal/recovery_test.cpp            recovered by replaying its journal
//   tests/journal/snapshot_test.cpp            saved and restored (it is Restorable)
//
// The differential test is the strict one: market data equal to the
// reference's byte for byte and reports equal one by one, so the order in
// which things are published is part of the contract. The builders in
// market_data.hpp make the messages, so the two engines cannot differ on a
// field nobody meant to vary.
//
//   build/release/bench/engine_bench --engine reference
//   build/release/bench/engine_bench --engine pooled
//
// DESIGN
//
//  1. The order node: one cache line.
//     An order holds its two neighbours and its level (so cancel removes it
//     without searching, and keeps the level's total right), then what the
//     matching loop and the reports read on every trade: id, token, owner,
//     displayed shares, and the two instructions that stay with it. That is
//     58 bytes; the node is aligned to 64, so each order is exactly one cache
//     line and a trade against it touches one line. What only icebergs need
//     (the hidden reserve, the display size, the second number the market
//     knows the order by) is in a separate Iceberg record (point 9), reached
//     through one pointer that is null for every ordinary order. The book an
//     order belongs to (locate and side) is kept once, in its level.
//
//  2. Doubly linked.
//     Matching only takes from the front, which a singly linked list does
//     well. cancel() takes from anywhere: with a back pointer that is two
//     pointer writes; without one it is a walk from the head of the level to
//     find the predecessor, which on a deep level is the whole queue.
//
//  3. Orders live in a util::Pool.
//     Stable addresses (the list and the index hold pointers), no call to the
//     general allocator once warm, and the slot of the order that just left
//     is the next one handed out, while it is still in cache. Iceberg records
//     come from a second pool.
//
//  4. The id index: an array, in chunks, with the dead prefix given back.
//     The engine hands out ids itself, 1, 2, 3, ... with no gaps, so the
//     cheapest map from an id to its order is an array indexed by the id:
//     one load, no hashing, no probing. One array for a whole day would keep
//     a pointer for every id ever issued (800 MB after a hundred million
//     orders, nearly all long gone). So the array is split into chunks of
//     4096 ids, allocated when first needed and freed as soon as every id in
//     them has been issued and none of their orders is still resting. Memory
//     then follows the span of ids that are alive, not the count of ids ever
//     given out. Ids from outside (0, an id not issued yet, 2^64 - 1) fall
//     outside every allocated chunk and are "unknown order", never an
//     out-of-bounds read.
//
//  5. The levels: std::map<Price, Level> per side.
//     Kept from the reference on purpose, so the comparison between the two
//     engines measures the orders, the pool and the index and nothing else. A
//     std::map node never moves, so an order can point at its level. If the
//     levels moved to a contiguous array (phase 4's finding applies here
//     too), an order could not hold a pointer that survives inserts; it would
//     hold its price and find the level when it needs it, which a cancel does
//     once.
//
//  6. Empty levels are erased at once.
//     The contract's walks never show an empty level, and best() is then
//     always the first entry. Keeping empty levels would save the tree node
//     when a price comes back, at the cost of skipping empties in best(), in
//     the matching loop and in every walk, and of a structure that grows all
//     day unless something cleans it.
//
//  7. Fill-or-kill: a walk over level totals, before the first trade.
//     A trade cannot be taken back once published, so the check comes first.
//     Each level keeps its displayed total and its hidden total, so the check
//     visits levels, not orders, and stops as soon as enough is found.
//     Self-match prevention is the exception (point 9).
//
//  8. Allocation after warm-up.
//     The reference allocates per resting order a list node, an index node,
//     and sometimes a tree node. Here a resting order allocates nothing: its
//     node is a recycled pool slot, its index entry a slot in a chunk that
//     serves 4096 orders. What is left is one tree node when a price level
//     that did not exist appears, and a new index chunk every 4096 ids.
//
//  9. The instructions (post-only, icebergs, self-match prevention).
//     An ordinary order does not pay for them: the node carries post_only and
//     the self-match mode (two bytes it had room for) and a null Iceberg
//     pointer, and every iceberg-only field is behind that pointer. A new
//     slice of an iceberg goes to the back of its level: unlink the head,
//     link a tail, no allocation; when the order is alone in its level it is
//     head and tail at once and nothing moves. The slice takes the next id
//     as its market reference, so the id counter moves without an order
//     arriving, and the index has nothing at that number: cancel(owner,
//     that_number) is "unknown order", because orders are named by their id,
//     never by the reference the market data uses. With icebergs,
//     fill-or-kill counts each level's hidden total as well. With
//     self-match prevention no total can answer it, because what can trade
//     depends on whose orders are where in the level: then, and only then,
//     the check walks the orders of each level. Removing a resting order
//     (cancel, complete fill, self-match) is one function, unlink().
//
// 10. Restorable (journal/snapshot.hpp): the five functions at the end put
//     instruments, orders and counters back without publishing anything, so
//     a snapshot of this engine can be saved and loaded like the reference's.
//     snapshot.hpp refuses an order id above the snapshot's own counter, so
//     the index's list of chunks comes back as long as the running engine's
//     was: one pointer per 4096 ids issued. A file this engine never wrote,
//     claiming huge ids, gets a list as long as it claims (8 bytes per 4096
//     ids), or std::bad_alloc when that is more than the machine has; it
//     never makes the index read out of bounds.

namespace obe::engine {

template <ReportSink Reports, MarketDataSink MarketData>
class MatchingEngine {
 public:
    static constexpr std::size_t kLocates = std::size_t{1} << 16;

    // Sinks are held by reference and must outlive the engine.
    MatchingEngine(Reports& reports, MarketData& market_data)
        : reports_(&reports), md_(&market_data), books_(kLocates) {}

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // Orders and iceberg records are trivially destructible pool storage:
    // the pools free their slabs, and nothing else needs to run.
    ~MatchingEngine() = default;

    // --- Operations (contract: concepts.hpp) ---------------------------------

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

    OrderId submit(const NewOrder& o, Nanos now) {
        Book& book = books_[o.locate];
        if (!book.listed) {
            return reject(o.owner, o.token, 0, RejectReason::UnknownInstrument, now);
        }
        if (!valid_side(o.side)) {
            return reject(o.owner, o.token, 0, RejectReason::BadSide, now);
        }
        if (o.qty == 0) {
            return reject(o.owner, o.token, 0, RejectReason::ZeroQuantity, now);
        }
        const bool is_limit = o.kind == OrderKind::Limit;
        if (is_limit && o.price == 0) {
            return reject(o.owner, o.token, 0, RejectReason::ZeroPrice, now);
        }
        const bool day = is_limit && o.tif == TimeInForce::Day;
        if (!day && (o.post_only || (o.display != 0 && o.display < o.qty))) {
            return reject(o.owner, o.token, 0, RejectReason::BadInstruction, now);
        }

        const OrderId id = next_id();
        ++stats_.accepted;
        reports_->on_accepted(Accepted{id, o.owner, o.token, o.locate, o.side, o.qty,
                                       is_limit ? o.price : Price{0}, o.kind, o.tif, now});

        const Price limit = effective_limit(o);
        const Incoming in{id,          o.owner,
                          o.token,     o.locate,
                          o.side,      day ? o.display : Qty{0},
                          o.post_only, known(o.self_match)};
        if (o.post_only && would_trade(book, o.side, o.price)) {
            drop(in, o.qty, CancelReason::PostOnly, now);
            return id;
        }
        if (o.tif == TimeInForce::FillOrKill && reachable(book, in, limit, o.qty) < o.qty) {
            drop(in, o.qty, CancelReason::FillOrKill, now);
            return id;
        }
        Qty left = o.qty;
        if (!match(book, in, limit, left, now) || left == 0) {
            return id;
        }
        if (day) {
            const Qty shown = rest(book, in, o.price, left);
            md_->on_add(md::add(o.locate, now, id, o.side, shown, book.symbol, o.price));
        } else {
            drop(in, left, is_limit ? CancelReason::ImmediateOrCancel : CancelReason::NoLiquidity,
                 now);
        }
        return id;
    }

    bool cancel(OwnerId owner, OrderId id, Nanos now) {
        Order* order = index_.find(id);
        if (order == nullptr) {
            reject(owner, 0, id, RejectReason::UnknownOrder, now);
            return false;
        }
        if (order->owner != owner) {
            reject(owner, 0, id, RejectReason::NotOwner, now);
            return false;
        }
        const Locate locate = order->level->locate;
        const Token token = order->token;
        const Qty qty = open(*order);
        const OrderId ref = market_ref(*order);
        unlink(order);
        ++stats_.cancels;
        md_->on_delete(md::remove(locate, now, ref));
        reports_->on_cancelled(Cancelled{id, owner, token, qty, 0, CancelReason::Requested, now});
        return true;
    }

    OrderId replace(OwnerId owner, OrderId id, Qty new_qty, Price new_price, Nanos now) {
        Order* order = index_.find(id);
        if (order == nullptr) {
            return reject(owner, 0, id, RejectReason::UnknownOrder, now);
        }
        if (order->owner != owner) {
            return reject(owner, 0, id, RejectReason::NotOwner, now);
        }
        if (new_qty == 0) {
            return reject(owner, 0, id, RejectReason::ZeroQuantity, now);
        }
        if (new_price == 0) {
            return reject(owner, 0, id, RejectReason::ZeroPrice, now);
        }
        Level& level = *order->level;
        const Locate locate = level.locate;
        const Side side = level.side;
        const Token token = order->token;
        Book& book = books_[locate];
        if (order->post_only && would_trade(book, side, new_price)) {
            return reject(owner, 0, id, RejectReason::WouldTrade, now);
        }
        ++stats_.replaces;

        const Qty open_qty = open(*order);
        if (new_price == level.price && new_qty <= open_qty) {
            // Same price, no larger: it keeps its place. The reserve goes
            // first, so what the market sees changes as little as possible.
            Qty off = open_qty - new_qty;
            if (order->ice != nullptr) {
                const Qty unseen = std::min(off, order->ice->hidden);
                order->ice->hidden -= unseen;
                level.hidden -= unseen;
                off -= unseen;
            }
            if (off != 0) {
                order->qty -= off;
                level.total -= off;
                md_->on_cancel(md::reduce(locate, now, market_ref(*order), off));
            }
            reports_->on_replaced(Replaced{id, id, owner, token, new_qty, new_price, true, now});
            return id;
        }

        // It loses its place: out under its old id, in under a new one.
        Incoming fresh{
            0, owner, token, locate, side, display(*order), order->post_only, order->self_match};
        const OrderId old_ref = market_ref(*order);
        unlink(order);
        fresh.id = next_id();
        reports_->on_replaced(Replaced{id, fresh.id, owner, token, new_qty, new_price, false, now});
        if (!would_trade(book, side, new_price)) {
            const Qty shown = rest(book, fresh, new_price, new_qty);
            md_->on_replace(md::replace(locate, now, old_ref, fresh.id, shown, new_price));
            return fresh.id;
        }
        md_->on_delete(md::remove(locate, now, old_ref));
        Qty left = new_qty;
        if (match(book, fresh, new_price, left, now) && left != 0) {
            const Qty shown = rest(book, fresh, new_price, left);
            md_->on_add(md::add(locate, now, fresh.id, side, shown, book.symbol, new_price));
        }
        return fresh.id;
    }

    // --- Queries -------------------------------------------------------------

    [[nodiscard]] std::optional<book::Level> best(Locate locate, Side side) const {
        const Ladder& ladder = books_[locate].side(side);
        if (ladder.empty()) {
            return std::nullopt;
        }
        const Level& top =
            side == Side::Buy ? std::prev(ladder.end())->second : ladder.begin()->second;
        return book::Level{top.price, top.total};
    }

    // f(const book::Level&) -> bool, best to worst; false stops the walk.
    template <class F>
    void for_each_level(Locate locate, Side side, F&& f) const {
        walk(books_[locate].side(side), side,
             [&f](const Level& level) { return f(book::Level{level.price, level.total}); });
    }

    // f(const RestingOrder&) -> bool, in the order they would trade.
    template <class F>
    void for_each_order(Locate locate, Side side, F&& f) const {
        walk(books_[locate].side(side), side, [&f](const Level& level) {
            for (const Order* o = level.head; o != nullptr; o = o->next) {
                RestingOrder shown{o->id, o->owner, o->token, level.price, o->qty};
                if (o->ice != nullptr) {
                    shown.hidden = o->ice->hidden;
                    shown.display = o->ice->display;
                    shown.ref = o->ice->ref == o->id ? OrderId{0} : o->ice->ref;
                }
                shown.post_only = o->post_only;
                shown.self_match = o->self_match;
                if (!f(shown)) {
                    return false;
                }
            }
            return true;
        });
    }

    [[nodiscard]] std::size_t open_orders() const noexcept { return orders_.in_use(); }

    [[nodiscard]] bool listed(Locate locate) const noexcept { return books_[locate].listed; }

    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }

    // --- Restoring (concepts.hpp, "Restoring") -------------------------------

    [[nodiscard]] feed::Symbol symbol(Locate locate) const noexcept {
        return books_[locate].symbol;
    }

    [[nodiscard]] EngineCounters counters() const noexcept {
        return {index_.issued(), last_match_};
    }

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
            index_.find(order.id) != nullptr) {
            return false;
        }
        Level& level = level_at(book, side, locate, order.price);
        Iceberg* ice = nullptr;
        if (order.display != 0 || order.hidden != 0 || order.market_ref() != order.id) {
            ice =
                new (icebergs_.allocate()) Iceberg{order.hidden, order.display, order.market_ref()};
        }
        Order* node = new (orders_.allocate())
            Order{level.tail, nullptr,     &level,    order.id,        order.token,
                  ice,        order.owner, order.qty, order.post_only, known(order.self_match)};
        link_back(level, node);
        level.total += order.qty;
        level.hidden += order.hidden;
        index_.set(order.id, node);
        return true;
    }

    void restore_counters(const EngineCounters& counters, const EngineStats& stats) {
        index_.set_issued(counters.last_order_id);
        last_match_ = counters.last_match_number;
        stats_ = stats;
    }

 private:
    struct Level;

    // What only an iceberg needs (design point 9).
    struct Iceberg {
        Qty hidden;   // the reserve
        Qty display;  // the display size it was given
        OrderId ref;  // what the market data calls it: the id until replenished
    };

    // One resting order, one cache line (design point 1).
    struct alignas(64) Order {
        Order* prev;
        Order* next;
        Level* level;
        OrderId id;
        Token token;
        Iceberg* ice;  // null for an order with no display size
        OwnerId owner;
        Qty qty;  // displayed and open
        bool post_only;
        SelfMatch self_match;
    };
    static_assert(sizeof(Order) == 64, "an order was meant to fill exactly one cache line");

    struct Level {
        Price price = 0;
        Locate locate = 0;
        Side side = Side::Buy;
        std::uint64_t total = 0;   // displayed shares
        std::uint64_t hidden = 0;  // reserve shares, which fill-or-kill may count
        Order* head = nullptr;     // oldest
        Order* tail = nullptr;     // newest
    };

    using Ladder = std::map<Price, Level>;

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

    // An order arriving: what the matching loop and rest() need to know.
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

    // Order id to order (design point 4).
    class IdIndex {
     public:
        [[nodiscard]] Order* find(OrderId id) const noexcept {
            const std::uint64_t c = id >> kChunkBits;
            if (c >= chunks_.size() || chunks_[c] == nullptr) {
                return nullptr;
            }
            return chunks_[c]->slots[id & kChunkMask];
        }

        void set(OrderId id, Order* order) {
            Chunk& chunk = chunk_for(id);
            chunk.slots[id & kChunkMask] = order;
            ++chunk.live;
        }

        void clear(OrderId id) noexcept {
            const std::uint64_t c = id >> kChunkBits;
            Chunk& chunk = *chunks_[c];
            chunk.slots[id & kChunkMask] = nullptr;
            --chunk.live;
            release_if_done(c);
        }

        // The next id, which nothing is indexed under yet.
        OrderId issue() noexcept {
            ++issued_;
            if ((issued_ & kChunkMask) == 0) {
                // The previous chunk has now been fully issued.
                release_if_done((issued_ >> kChunkBits) - 1);
            }
            return issued_;
        }

        [[nodiscard]] OrderId issued() const noexcept { return issued_; }
        void set_issued(OrderId id) noexcept { issued_ = id; }

     private:
        static constexpr unsigned kChunkBits = 12;
        static constexpr std::uint64_t kChunkMask = (std::uint64_t{1} << kChunkBits) - 1;

        struct Chunk {
            std::array<Order*, std::size_t{1} << kChunkBits> slots{};
            std::uint32_t live = 0;
        };

        Chunk& chunk_for(OrderId id) {
            const auto c = static_cast<std::size_t>(id >> kChunkBits);
            if (c >= chunks_.size()) {
                chunks_.resize(c + 1);
            }
            if (chunks_[c] == nullptr) {
                chunks_[c] = std::make_unique<Chunk>();
            }
            return *chunks_[c];
        }

        // A chunk none of whose orders rests, and none of whose ids is still
        // to be issued, can never be looked at again.
        void release_if_done(std::uint64_t c) noexcept {
            if (c < chunks_.size() && chunks_[c] != nullptr && chunks_[c]->live == 0 &&
                ((c + 1) << kChunkBits) - 1 <= issued_) {
                chunks_[c].reset();
            }
        }

        std::vector<std::unique_ptr<Chunk>> chunks_;
        OrderId issued_ = 0;
    };

    [[nodiscard]] static SelfMatch known(SelfMatch m) noexcept {
        switch (m) {
            case SelfMatch::CancelIncoming:
            case SelfMatch::CancelResting:
            case SelfMatch::CancelBoth:
                return m;
            default:
                return SelfMatch::Allow;
        }
    }

    [[nodiscard]] static Qty hidden(const Order& o) noexcept {
        return o.ice != nullptr ? o.ice->hidden : 0;
    }
    [[nodiscard]] static Qty display(const Order& o) noexcept {
        return o.ice != nullptr ? o.ice->display : 0;
    }
    [[nodiscard]] static OrderId market_ref(const Order& o) noexcept {
        return o.ice != nullptr ? o.ice->ref : o.id;
    }
    // Open shares, seen or not, as a report states them.
    [[nodiscard]] static Qty open(const Order& o) noexcept { return o.qty + hidden(o); }

    // Calls f(const Level&) best to worst; false stops the walk.
    template <class F>
    static void walk(const Ladder& ladder, Side side, F&& f) {
        if (side == Side::Buy) {
            for (auto it = ladder.rbegin(); it != ladder.rend(); ++it) {
                if (!f(it->second)) {
                    return;
                }
            }
        } else {
            for (const auto& entry : ladder) {
                if (!f(entry.second)) {
                    return;
                }
            }
        }
    }

    OrderId next_id() noexcept { return index_.issue(); }

    OrderId reject(OwnerId owner, Token token, OrderId id, RejectReason reason, Nanos now) {
        ++stats_.rejected;
        reports_->on_rejected(Rejected{owner, token, id, reason, now});
        return 0;
    }

    // The engine cancels what is left of an arriving order.
    void drop(const Incoming& in, Qty shares, CancelReason reason, Nanos now) {
        stats_.unfilled_shares += shares;
        reports_->on_cancelled(Cancelled{in.id, in.owner, in.token, shares, 0, reason, now});
    }

    [[nodiscard]] static bool would_trade(const Book& book, Side side, Price limit) {
        const Side other = opposite(side);
        const Ladder& ladder = book.side(other);
        if (ladder.empty()) {
            return false;
        }
        const Price top =
            other == Side::Buy ? std::prev(ladder.end())->first : ladder.begin()->first;
        return acceptable(side, limit, top);
    }

    // Shares the arriving order could trade, counting stops once `want` is
    // reached (design points 7 and 9).
    [[nodiscard]] static std::uint64_t reachable(const Book& book, const Incoming& in, Price limit,
                                                 Qty want) {
        const Side other = opposite(in.side);
        std::uint64_t n = 0;
        bool blocked = false;
        walk(book.side(other), other, [&](const Level& level) {
            if (blocked || !acceptable(in.side, limit, level.price)) {
                return false;
            }
            if (in.self_match == SelfMatch::Allow) {
                n += level.total + level.hidden;
                return n < want;
            }
            if (in.self_match == SelfMatch::CancelResting) {
                // The owner's own orders go; everyone else's can all trade.
                for (const Order* o = level.head; o != nullptr; o = o->next) {
                    if (o->owner != in.owner) {
                        n += open(*o);
                    }
                }
                return n < want;
            }
            // Cancel the arriving order (or both): trading stops at the
            // owner's first order. Reserve shares of a level that holds one
            // come back behind it, so only what shows ahead of it counts.
            const Order* own = level.head;
            while (own != nullptr && own->owner != in.owner) {
                own = own->next;
            }
            if (own == nullptr) {
                n += level.total + level.hidden;
                return n < want;
            }
            for (const Order* o = level.head; o != own; o = o->next) {
                n += o->qty;
            }
            blocked = true;
            return false;
        });
        return n;
    }

    // Trades the arriving order against the other side. Returns false if
    // self-match prevention stopped it (what was left is then cancelled).
    bool match(Book& book, const Incoming& in, Price limit, Qty& qty, Nanos now) {
        const Side other = opposite(in.side);
        Ladder& ladder = book.side(other);
        while (qty != 0 && !ladder.empty()) {
            Level& level =
                other == Side::Buy ? std::prev(ladder.end())->second : ladder.begin()->second;
            const Price price = level.price;
            if (!acceptable(in.side, limit, price)) {
                break;
            }
            Order* resting = level.head;
            if (in.self_match != SelfMatch::Allow && resting->owner == in.owner) {
                ++stats_.self_matches;
                const bool cancel_resting = in.self_match != SelfMatch::CancelIncoming;
                const bool stop = in.self_match != SelfMatch::CancelResting;
                if (cancel_resting) {
                    const Cancelled gone{resting->id,
                                         resting->owner,
                                         resting->token,
                                         open(*resting),
                                         0,
                                         CancelReason::SelfMatch,
                                         now};
                    const OrderId ref = market_ref(*resting);
                    unlink(resting);
                    md_->on_delete(md::remove(in.locate, now, ref));
                    reports_->on_cancelled(gone);
                }
                if (stop) {
                    drop(in, qty, CancelReason::SelfMatch, now);
                    return false;
                }
                continue;
            }

            const Qty fill = std::min(qty, resting->qty);
            const std::uint64_t match_number = ++last_match_;
            resting->qty -= fill;
            level.total -= fill;
            qty -= fill;
            ++stats_.trades;
            stats_.traded_shares += fill;
            md_->on_execute(md::execute(in.locate, now, market_ref(*resting), fill, match_number));
            reports_->on_executed(Executed{resting->id, resting->owner, resting->token, fill, price,
                                           open(*resting), match_number, Liquidity::Added, now});
            reports_->on_executed(Executed{in.id, in.owner, in.token, fill, price, qty,
                                           match_number, Liquidity::Removed, now});
            if (resting->qty != 0) {
                continue;
            }
            if (hidden(*resting) == 0) {
                unlink(resting);
                continue;
            }
            // The next slice of an iceberg: to the back, under a new reference.
            Iceberg& ice = *resting->ice;
            const Qty slice = std::min(ice.display, ice.hidden);
            resting->qty = slice;
            ice.hidden -= slice;
            level.hidden -= slice;
            level.total += slice;
            ice.ref = next_id();
            if (level.tail != resting) {
                level.head = resting->next;
                level.head->prev = nullptr;
                resting->next = nullptr;
                link_back(level, resting);
            }
            md_->on_add(md::add(in.locate, now, ice.ref, other, slice, book.symbol, price));
        }
        return true;
    }

    // The level at `price`, created empty if needed.
    Level& level_at(Book& book, Side side, Locate locate, Price price) {
        Level& level = book.side(side)[price];
        level.price = price;
        level.locate = locate;
        level.side = side;
        return level;
    }

    static void link_back(Level& level, Order* order) noexcept {
        order->prev = level.tail;
        order->next = nullptr;
        if (level.tail != nullptr) {
            level.tail->next = order;
        } else {
            level.head = order;
        }
        level.tail = order;
    }

    // Puts what is left of an arriving order in the book. Returns the shares
    // displayed.
    Qty rest(Book& book, const Incoming& in, Price price, Qty qty) {
        const Qty shown = in.display != 0 && in.display < qty ? in.display : qty;
        Level& level = level_at(book, in.side, in.locate, price);
        Iceberg* ice = nullptr;
        if (in.display != 0) {
            ice = new (icebergs_.allocate()) Iceberg{qty - shown, in.display, in.id};
        }
        auto* order =
            new (orders_.allocate()) Order{nullptr, nullptr,  &level, in.id,        in.token,
                                           ice,     in.owner, shown,  in.post_only, in.self_match};
        link_back(level, order);
        level.total += shown;
        level.hidden += qty - shown;
        index_.set(in.id, order);
        return shown;
    }

    // Takes a resting order out of its level and the index, and frees it
    // (design point 9: the one place an order leaves the book). Erases the
    // level if it is now empty (design point 6).
    void unlink(Order* order) {
        Level* level = order->level;
        (order->prev != nullptr ? order->prev->next : level->head) = order->next;
        (order->next != nullptr ? order->next->prev : level->tail) = order->prev;
        level->total -= order->qty;
        if (order->ice != nullptr) {
            level->hidden -= order->ice->hidden;
            icebergs_.deallocate(order->ice);
        }
        index_.clear(order->id);
        orders_.deallocate(order);
        if (level->head == nullptr) {
            // Copies: the key must not live in the node being erased.
            const Price price = level->price;
            const Side side = level->side;
            books_[level->locate].side(side).erase(price);
        }
    }

    Reports* reports_;
    MarketData* md_;
    std::vector<Book> books_;
    IdIndex index_;
    util::Pool<Order> orders_;
    util::Pool<Iceberg> icebergs_{1024};
    EngineStats stats_{};
    std::uint64_t last_match_ = 0;
};

}  // namespace obe::engine
