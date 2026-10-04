#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

#include "obe/book/concepts.hpp"
#include "obe/book/types.hpp"
#include "obe/types.hpp"

// The reference order store: a map from order reference number to OrderRecord,
// built on std::unordered_map.
//
// It exists to be obviously correct and to be the baseline every later number
// is compared with. Its contract is OrderStoreLike in obe/book/concepts.hpp and
// its judge is tests/book/order_store_test.cpp.
//
// Why this is the hot data structure. An Order Executed message carries a
// reference number and a share count, nothing else. The price and side needed
// to update the right level have to be looked up here. The same is true of
// cancels, deletes and replaces, so nearly every message in the feed costs one
// find() in this map.
//
// Why std::unordered_map is slow at that job, which is what phase 4 measures:
//   - Every order is a separately allocated node. An add calls the allocator
//     and a delete calls it again.
//   - A lookup follows at least two pointers to memory that is rarely in
//     cache: the bucket array, then the node. With hundreds of thousands of
//     live orders the nodes are scattered across the heap in allocation order,
//     which has nothing to do with lookup order.
//   - Growing rehashes every element at once, which shows up as a single very
//     large per-message latency.
//
// The decisions in this file:
//
//   insert uses try_emplace. It inserts only when the key is absent and says
//   which case happened, in one lookup. operator[] would default-construct a
//   record for a missing key and silently overwrite an existing one; both are
//   wrong here, because a duplicate reference number must be reported and the
//   original kept.
//
//   find returns a raw pointer, nullptr for "not there". The contract promises
//   it only until the next insert or erase. std::unordered_map gives more (a
//   rehash moves buckets, never nodes), but an open-addressing table that
//   grows does move its elements, and BookManager must keep working unchanged
//   when the container is swapped. A contract is set by the weakest
//   implementation that has to honour it.
//
//   The constructor can reserve. A day has hundreds of millions of adds, but
//   what sizes the table is the peak number of orders alive at once, which
//   feed_profile reports. For this node-based map, reserve() pre-builds the
//   bucket array and so avoids every rehash; it cannot avoid the per-order
//   node allocation.
//
//   The allocator is a template parameter so that the pool-allocator
//   experiment (obe/book/pooled.hpp) can reuse this class unchanged.

namespace obe::book {

template <class Allocator = std::allocator<std::pair<const OrderId, OrderRecord>>>
class BasicOrderStore {
 public:
    BasicOrderStore() = default;

    // Pre-size for `expected_orders` live orders, so the table never rehashes
    // until that many are resting at once.
    explicit BasicOrderStore(std::size_t expected_orders) { map_.reserve(expected_orders); }

    // Adds the order. Returns false, changing nothing, if `id` is present.
    bool insert(OrderId id, const OrderRecord& rec) { return map_.try_emplace(id, rec).second; }

    // The stored record, or nullptr. Valid until the next insert or erase.
    [[nodiscard]] OrderRecord* find(OrderId id) noexcept {
        const auto it = map_.find(id);
        return it == map_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const OrderRecord* find(OrderId id) const noexcept {
        const auto it = map_.find(id);
        return it == map_.end() ? nullptr : &it->second;
    }

    // Removes the order. Returns false if it was not present.
    bool erase(OrderId id) noexcept { return map_.erase(id) != 0; }

    // Number of orders currently stored.
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

 private:
    std::unordered_map<OrderId, OrderRecord, std::hash<OrderId>, std::equal_to<OrderId>, Allocator>
        map_;
};

using OrderStore = BasicOrderStore<>;

static_assert(OrderStoreLike<OrderStore>);

}  // namespace obe::book
