#pragma once

#include <cstddef>

#include "obe/util/cache_line.hpp"
#include "obe/util/queue_concepts.hpp"
#include "obe/util/todo.hpp"

// ============================================================================
//  YOURS TO WRITE: phase 6, the lock-free queue
// ============================================================================
//
// SpscRing<T> is a bounded queue for exactly one producer thread and exactly
// one consumer thread, with no lock. The contract is BoundedQueue in
// queue_concepts.hpp: try_push and try_pop, neither of which ever waits.
//
// The spec asks for four things by name: a power-of-two capacity, head and
// tail on separate cache lines, acquire and release memory ordering, and a
// cached copy of the other side's index. Each is a decision below, and each
// should be something you can explain at a whiteboard.
//
// The judges, labelled needs-your-code until this is written:
//
//   tests/util/queue_test.cpp      the BoundedQueue contract, one thread and two
//   tests/util/channel_test.cpp    the ring behind SpinChannel
//   tests/pipeline/pipeline_test.cpp   the three-stage pipeline on rings
//
// and the two that criterion 6 names:
//
//   cmake --preset tsan && cmake --build --preset tsan
//   ctest --preset tsan -L needs-your-code            # ThreadSanitizer must be silent
//   scripts/queue_stress.sh build/release/bench/queue_bench ring   # one billion, in order
//
// The baseline to beat is MutexQueue (mutex_queue.hpp):
//
//   build/release/bench/queue_bench --queue mutex
//   build/release/bench/queue_bench --queue mutex-spin
//   build/release/bench/queue_bench --queue ring
//
// Decisions to make:
//
//  1. Two indices, each with one writer.
//     The producer is the only thread that moves the tail; the consumer is the
//     only thread that moves the head. Convince yourself that this alone
//     removes the need for a lock or a compare-and-swap. What would break
//     with a second producer?
//
//  2. Why a power of two.
//     If the indices are free-running counters that never wrap back to zero
//     (64 bits will not overflow in any run that finishes), the slot is
//     `index & (capacity - 1)` and the number of items is `tail - head`. What
//     does a general capacity cost on every operation instead? And how do you
//     tell full from empty: is a wasted slot needed?
//
//  3. Which memory order, and why.
//     The consumer must not read a slot before the producer has finished
//     writing it. The store that publishes the new tail and the load that
//     observes it are what guarantee that. Which of them is release and which
//     acquire? What may the producer use to load its own tail, which nobody
//     else writes? Why is seq_cst more than this needs, and what does it cost
//     on x86? Note that x86 will run a ring with every order relaxed without
//     ever failing: its hardware is stricter than the language. That is what
//     makes ThreadSanitizer, not a stress run, the test of this point.
//
//  4. False sharing.
//     The head is written by one core and the tail by another. If they share a
//     cache line, every operation steals the line from the other core. Put
//     each on a line of its own (alignas(kCacheLine)). Then look at what else
//     is on those lines: the buffer pointer and the mask are read by both
//     sides and written by neither. Where should they live?
//
//  5. The cached copy of the other side's index.
//     To push, the producer needs to know there is room, which means reading
//     the head, which lives on a line the consumer keeps writing. But the
//     head only ever moves forward: an old value of it can only make the
//     queue look fuller than it is. So the producer can keep a private copy
//     and consult the real head only when the copy says "full". How often is
//     the real head read then, when the queue is mostly empty? Mirror it for
//     the consumer. Which line does each private copy belong on?
//     This is the one that is worth measuring: build it without the cached
//     copies first, run queue_bench, add them, run it again, and log both.
//
//  6. The slots.
//     The contract says T is default-constructible and move-assignable, which
//     lets the buffer be a plain array of T that exists from the start.
//     try_pop then leaves a moved-from T in the slot. What does that keep
//     alive for a std::string or a std::unique_ptr, and until when? The
//     alternative is raw storage with placement new and an explicit destructor
//     call; what would the destructor of the ring have to do then?
//
//  7. On failure, hands off.
//     try_push must leave the value untouched when the queue is full, because
//     the caller will try again with the same object. Check the full test
//     before anything is moved.
//
//  8. What "wait-free" means here.
//     Each operation finishes in a bounded number of steps whatever the other
//     thread does: there is no loop in try_push or try_pop. Compare that with
//     "lock-free", and say which of the two a compare-and-swap retry loop is.

namespace obe::util {

template <class T>
class SpscRing {
 public:
    using value_type = T;

    // A ring that holds at least `capacity` items (at least 1), rounded up to
    // a power of two.
    explicit SpscRing(std::size_t capacity) { static_cast<void>(capacity); }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer thread only. False if the ring is full; value is then untouched.
    template <class U>
    [[nodiscard]] bool try_push(U&& value) {
        todo("SpscRing::try_push", value);
    }

    // Consumer thread only. False if the ring is empty; out is then untouched.
    [[nodiscard]] bool try_pop(T& out) { todo("SpscRing::try_pop", out); }

    // The number of slots: a power of two, at least what was asked for.
    [[nodiscard]] std::size_t capacity() const { todo("SpscRing::capacity"); }

 private:
    // Your indices, their cached copies and the buffer go here.
};

}  // namespace obe::util
