#pragma once

#include <cstddef>

#include "obe/book/concepts.hpp"
#include "obe/book/types.hpp"
#include "obe/types.hpp"
#include "obe/util/todo.hpp"

// ============================================================================
//  YOURS TO WRITE: phase 4, experiment 1
// ============================================================================
//
// FlatOrderStore is the optimized order store: an open-addressing hash table
// that keeps every order in one contiguous, pre-sized array instead of one heap
// node per order.
//
// The hypothesis (spec, phase 4): std::unordered_map allocates a node per order
// and chases two pointers per lookup; a flat table removes the allocation and
// turns most lookups into one cache line. Write the hypothesis and your
// expected numbers in docs/optimization-log.md BEFORE measuring.
//
// The contract is OrderStoreLike (obe/book/concepts.hpp), the same one the
// reference satisfies, so BookManager and every test work unchanged. The judges:
//
//   tests/book/order_store_test.cpp     the container contract and a model test
//   tests/book/differential_test.cpp    the whole book against the reference
//   scripts/diff_books.sh               the same check on a full real day
//   replay_bench --impl flat-store      refuses to report timings as valid
//                                       unless the hash matches the reference
//
// Every function below throws until you replace its body.
//
// Decisions to make. Measure where you can; `build/release/apps/feed_profile
// <file>` reports the facts about the real feed that several of them turn on.
//
//  1. Collisions: where does an order go when its slot is taken?
//     Linear probing keeps a probe sequence inside a cache line or two.
//     Quadratic probing and double hashing spread clusters out and give up
//     that locality. Robin Hood hashing bounds the worst probe length by
//     moving entries on insert. Which costs the workload cares about: this is
//     a table where almost every operation is a find() that hits.
//
//  2. Deletion. This is the part most first attempts get wrong.
//     In ITCH nearly every order that is added is later removed, so the table
//     sees as many erases as inserts all day while its size stays roughly
//     level. With open addressing you cannot simply mark a slot empty: think
//     about a find() for a key whose probe sequence passed through that slot.
//     The two standard answers are tombstones and backward-shift deletion.
//     What happens to probe lengths after a few hundred million insert/erase
//     pairs with tombstones and no cleanup? (The churn test in
//     order_store_test.cpp exists to make that failure visible.)
//
//  3. How do you tell an empty slot from a used one?
//     A reserved key value is the cheapest, but the tests insert reference
//     numbers 0 and 2^64 - 1, and nothing in the specification rules either
//     out. A separate byte of metadata per slot costs memory and a second
//     array to touch. Is there spare room in the slot you already have?
//     (sizeof(OrderRecord) is 12; what does the compiler pad a slot to?)
//
//  4. The hash function.
//     feed_profile shows how reference numbers are distributed. If they are
//     close to sequential, the identity function with a power-of-two mask puts
//     consecutive orders in consecutive slots, which is either ideal or
//     terrible depending on your probing. What happens to it when keys differ
//     only in their high bits? (order_store_test.cpp has a test that does
//     exactly that.) A multiplicative hash is one multiply; is it worth it
//     here? Measure both and log the loser too.
//
//  5. Capacity and growth.
//     Power-of-two capacity makes "mod" a mask. What maximum load factor, and
//     what does probe length do as you approach it? feed_profile reports the
//     peak number of resting orders; pre-size from that so a real day never
//     grows. Growth must still work (the tests construct a small table and
//     overfill it), and it may move every element: that is why the contract
//     says a pointer from find() dies at the next insert or erase.
//
//  6. Layout (this is also experiment 5).
//     Array of {key, record} slots, or keys in one array and records in
//     another? During a probe you compare keys and nothing else. How many
//     keys fit in a cache line each way?
//
//  7. The class must stay movable: BookManager takes its store by value.
//
// Optional hooks for later experiments. Add them when you get there; the
// harness detects them at compile time and nothing breaks without them.
//
//   void prefetch(OrderId id) const noexcept;
//       Experiment 6. replay_bench --prefetch calls this for the order the
//       NEXT message refers to, before handling the current one. Issue a
//       software prefetch for the slot that id hashes to and return.
//
//   Experiment 8 (huge pages): obe/util/huge_pages.hpp has HugePageBuffer, a
//   block of memory that asks the kernel for transparent huge pages. Back the
//   slot array with it and compare.

namespace obe::book {

class FlatOrderStore {
 public:
    FlatOrderStore() = default;

    // Pre-size for `expected_orders` live orders.
    explicit FlatOrderStore(std::size_t expected_orders) { static_cast<void>(expected_orders); }

    // Adds the order. Returns false, changing nothing, if `id` is present.
    bool insert(OrderId id, const OrderRecord& rec) {
        util::todo("FlatOrderStore::insert", id, rec);
    }

    // The stored record, or nullptr. Valid until the next insert or erase.
    OrderRecord* find(OrderId id) { util::todo("FlatOrderStore::find", id); }
    const OrderRecord* find(OrderId id) const { util::todo("FlatOrderStore::find const", id); }

    // Removes the order. Returns false if it was not present.
    bool erase(OrderId id) { util::todo("FlatOrderStore::erase", id); }

    // Number of orders currently stored.
    std::size_t size() const { util::todo("FlatOrderStore::size"); }

 private:
    // Your storage goes here.
};

static_assert(OrderStoreLike<FlatOrderStore>);

}  // namespace obe::book
