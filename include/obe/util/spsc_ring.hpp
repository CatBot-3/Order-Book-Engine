#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "obe/util/cache_line.hpp"
#include "obe/util/queue_concepts.hpp"

// SpscRing<T>: a bounded queue for exactly one producer thread and exactly one
// consumer thread, with no lock. Phase 6. The contract is BoundedQueue in
// queue_concepts.hpp: try_push and try_pop, neither of which ever waits.
//
// The spec asks for four things by name: a power-of-two capacity, head and
// tail on separate cache lines, acquire and release memory ordering, and a
// cached copy of the other side's index. Each is a design point below.
//
// The judges:
//
//   tests/util/queue_test.cpp          the BoundedQueue contract, one thread and two
//   tests/util/channel_test.cpp        the ring behind SpinChannel
//   tests/pipeline/pipeline_test.cpp   the three-stage pipeline on rings
//
// and the two that criterion 6 names:
//
//   ctest --preset tsan -L hand-written          # ThreadSanitizer must be silent
//   scripts/queue_stress.sh build/release/bench/queue_bench ring   # one billion, in order
//
// The baseline it is measured against is MutexQueue (mutex_queue.hpp):
//
//   build/release/bench/queue_bench --queue mutex
//   build/release/bench/queue_bench --queue mutex-spin
//   build/release/bench/queue_bench --queue ring
//
// DESIGN
//
//  1. Two indices, each with one writer.
//     Only the producer moves the tail; only the consumer moves the head.
//     Each side reads the other's index and writes only its own, so there is
//     never a moment when two threads write one variable, which is what a
//     lock or a compare-and-swap would be there to settle. A second producer
//     would break that: two producers could both read tail = 7, both write
//     slot 7, and both publish 8, losing an item. Several producers need a
//     compare-and-swap on the tail (or one ring per producer).
//
//  2. A power of two, and free-running 64-bit indices.
//     The indices count every push and pop since the start and are never
//     wrapped back (2^64 operations would take centuries). The slot is
//     index & (capacity - 1), and the number of items is tail - head, exact
//     even when the queue is full: tail - head == capacity means full,
//     tail == head means empty, and no slot has to be left unused to tell
//     the two apart. A capacity that is not a power of two would need
//     index % capacity on every operation, a division costing tens of
//     cycles where the mask costs one.
//
//  3. Memory order: release to publish, acquire to observe.
//     The producer writes the slot, then stores the new tail with release:
//     nothing written before that store may become visible after it. The
//     consumer loads the tail with acquire: nothing it reads after that load
//     may be satisfied from before it. Together they guarantee that a
//     consumer which sees tail = 8 also sees the item written into slot 7.
//     The mirror image (consumer stores head with release after it has
//     moved the item out; producer loads head with acquire) guarantees the
//     producer never overwrites a slot the consumer is still reading. Each
//     side reads its own index relaxed: nobody else writes it. seq_cst would
//     add a single total order over all such operations that this queue does
//     not need; on x86 it makes every store a full fence (an xchg or mfence,
//     tens of cycles), where release and acquire are ordinary moves. x86
//     would run this ring correctly with every order relaxed, because its
//     hardware is stricter than the language: that is why ThreadSanitizer,
//     which checks the program against the language's rules, and not a
//     stress run, is the test of this point.
//
//  4. False sharing.
//     The tail is written by one core and the head by another. On one cache
//     line they would make the line bounce between the two cores on every
//     operation. So each index lives on its own line (alignas(kCacheLine)),
//     together with the private data of the side that writes it (point 5).
//     The buffer pointer and the mask are read by both sides and written by
//     neither: they live on a line of their own at the front, which both
//     cores can keep a shared copy of for ever.
//
//  5. A cached copy of the other side's index.
//     To push, the producer must know there is room, which means reading the
//     head, on a line the consumer keeps writing. But the head only moves
//     forward, so an old value can only make the queue look fuller than it
//     is, never emptier: safe. The producer keeps a private copy of the head
//     and reads the real one only when the copy says "full". When the
//     consumer keeps up, that happens about once per capacity's worth of
//     pushes instead of on every push. The consumer mirrors it with a cached
//     tail, consulted again only when the copy says "empty". Each private
//     copy sits on its owner's line, next to the index that owner writes.
//
//  6. The slots: a plain array of T that exists from the start.
//     The contract asks T to be default-constructible and move-assignable, so
//     the buffer is new T[capacity], and push and pop are move assignments.
//     try_pop leaves a moved-from T in the slot until a later push
//     overwrites it: a std::string keeps its heap buffer (if the move left
//     one), a std::unique_ptr nothing. The alternative, raw storage with
//     placement new and an explicit destructor in try_pop, would let a
//     popped item die at once, at the price of a destructor for the ring that
//     destroys exactly the items between head and tail.
//
//  7. On failure, hands off: try_push checks for room before it touches the
//     value, so a full queue leaves the caller's object as it was.
//
//  8. Wait-free.
//     Each operation finishes in a bounded number of its own steps whatever
//     the other thread does: there is no loop in try_push or try_pop.
//     Lock-free is the weaker promise that some thread always makes
//     progress; a compare-and-swap retry loop is lock-free but not wait-free,
//     because one thread can lose the race again and again.

namespace obe::util {

template <class T>
class SpscRing {
 public:
    using value_type = T;

    // A ring that holds at least `capacity` items (at least 1), rounded up to
    // a power of two.
    explicit SpscRing(std::size_t capacity)
        : mask_(std::bit_ceil(capacity == 0 ? std::size_t{1} : capacity) - 1),
          slots_(std::make_unique<T[]>(mask_ + 1)) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer thread only. False if the ring is full; value is then untouched.
    template <class U>
    [[nodiscard]] bool try_push(U&& value) {
        const std::uint64_t tail = producer_.tail.load(std::memory_order_relaxed);
        if (tail - producer_.head_cache > mask_) {
            // Looks full. Ask for the real head, once.
            producer_.head_cache = consumer_.head.load(std::memory_order_acquire);
            if (tail - producer_.head_cache > mask_) {
                return false;
            }
        }
        slots_[tail & mask_] = std::forward<U>(value);
        producer_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only. False if the ring is empty; out is then untouched.
    [[nodiscard]] bool try_pop(T& out) {
        const std::uint64_t head = consumer_.head.load(std::memory_order_relaxed);
        if (head == consumer_.tail_cache) {
            // Looks empty. Ask for the real tail, once.
            consumer_.tail_cache = producer_.tail.load(std::memory_order_acquire);
            if (head == consumer_.tail_cache) {
                return false;
            }
        }
        out = std::move(slots_[head & mask_]);
        consumer_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    // The number of slots: a power of two, at least what was asked for.
    [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

 private:
    // The producer's line: the index it writes, and its copy of the head.
    struct alignas(kCacheLine) Producer {
        std::atomic<std::uint64_t> tail{0};
        std::uint64_t head_cache = 0;
    };
    // The consumer's line: the index it writes, and its copy of the tail.
    struct alignas(kCacheLine) Consumer {
        std::atomic<std::uint64_t> head{0};
        std::uint64_t tail_cache = 0;
    };

    // Read by both, written by neither (design point 4).
    const std::size_t mask_;
    const std::unique_ptr<T[]> slots_;

    Producer producer_;
    Consumer consumer_;
};

}  // namespace obe::util
