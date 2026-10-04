#pragma once

#include <cstddef>

#include "obe/book/concepts.hpp"
#include "obe/book/types.hpp"
#include "obe/types.hpp"
#include "obe/util/todo.hpp"

// ============================================================================
//  YOURS TO WRITE (spec section 10: the order store is written by hand)
// ============================================================================
//
// OrderStore is the reference order store of phase 2: a map from order
// reference number to OrderRecord. The spec fixes the container for the
// reference version, std::unordered_map, because the reference exists to be
// obviously correct and to be the baseline every later number is compared with.
//
// The interface and its contract are in obe/book/concepts.hpp (OrderStoreLike).
// The judge is tests/book/order_store_test.cpp. Every function below currently
// throws; replace each util::todo(...) with the real body and add whatever
// private members you need.
//
// Things to settle before writing it. Each is also a question an interviewer
// can ask about this file, so it is worth having an answer in your own words.
//
//  1. Why is this the hot data structure at all?
//     Look at what an Order Executed message carries (feed/messages.hpp) and
//     what PriceLevels::remove needs. Roughly what fraction of a day's messages
//     end up calling find()? (itch_stats will tell you.)
//
//  2. insert() must not overwrite an existing order.
//     operator[] is the wrong tool: it does two things you do not want here.
//     Which member function inserts only when the key is absent and reports
//     which case happened, in a single lookup?
//
//  3. How long does the pointer from find() stay valid?
//     For std::unordered_map, does a rehash move the elements or only the
//     buckets? The concept still promises only "until the next insert or
//     erase", which is weaker than what this container gives. Why promise less
//     than you have? (Think about what an open-addressing table does when it
//     grows, and which class has to keep working unchanged in phase 4.)
//
//  4. Should the constructor reserve?
//     A day has hundreds of millions of adds, but what matters is the peak
//     number of orders alive at once. Measure it (track the maximum of size()
//     during a replay) before choosing a number. Then ask what reserve() saves
//     on a node-based map, and what it cannot save.
//
//  5. Which of these functions can be noexcept, and which cannot? Why does it
//     matter less here than it would for a move constructor?
//
// Deliberately not here yet: iteration, a custom hash, a pool allocator. Those
// are phase 4 experiments. Get the baseline measured first, so each of them
// has a "before" number.

namespace obe::book {

class OrderStore {
 public:
    OrderStore() = default;

    // Adds the order. Returns false, changing nothing, if `id` is present.
    bool insert(OrderId id, const OrderRecord& rec) { util::todo("OrderStore::insert", id, rec); }

    // The stored record, or nullptr. Valid until the next insert or erase.
    OrderRecord* find(OrderId id) { util::todo("OrderStore::find", id); }
    const OrderRecord* find(OrderId id) const { util::todo("OrderStore::find const", id); }

    // Removes the order. Returns false if it was not present.
    bool erase(OrderId id) { util::todo("OrderStore::erase", id); }

    // Number of orders currently stored.
    std::size_t size() const { util::todo("OrderStore::size"); }

 private:
    // Your storage goes here.
};

static_assert(OrderStoreLike<OrderStore>);

}  // namespace obe::book
