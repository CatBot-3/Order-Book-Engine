#pragma once

#include <concepts>
#include <cstddef>
#include <utility>

// The two contracts the pipeline's queues are written to.
//
// ---------------------------------------------------------------------------
// BoundedQueue: a fixed-capacity queue that never waits
// ---------------------------------------------------------------------------
//
//   Q(capacity)       A queue that holds at least `capacity` items.
//                     Precondition: capacity >= 1.
//   try_push(value)   Appends value and returns true, or returns false if the
//                     queue is full. On false, value is untouched: it has not
//                     been moved from, and the caller may try again with it.
//   try_pop(out)      Moves the oldest item into out and returns true, or
//                     returns false, leaving out alone, if the queue is empty.
//   capacity()        How many items fit. At least what was asked for; a queue
//                     may round up.
//
// Items come out in the order they went in.
//
// Threading: at most one thread calls try_push and at most one thread calls
// try_pop at any time. They may be different threads, running at the same
// moment. That is the whole of "single producer, single consumer", and it is
// what lets a queue get away without a lock.
//
// T must be default-constructible and move-assignable.
//
// ---------------------------------------------------------------------------
// Channel: a queue you can wait on, with an end
// ---------------------------------------------------------------------------
//
//   push(value)   Appends value, waiting while the queue is full. Returns
//                 false, with value not appended, only if the channel was
//                 cancelled.
//   pop(out)      Moves the oldest item into out, waiting while the queue is
//                 empty. Returns false when there will never be another item:
//                 the channel was closed and everything pushed before the
//                 close has been popped, or the channel was cancelled.
//   close()       The producer says it has pushed its last item. Items already
//                 in the queue are still delivered.
//   cancel()      Either side says "stop now". Both push and pop return false
//                 from then on, waiting or not. Items in the queue are
//                 abandoned. Unlike everything else here, cancel may be called
//                 from any thread: it is how a failing stage of the pipeline
//                 unblocks its neighbours.
//   cancelled()   Whether cancel was called.
//   stats()       ChannelStats: how often each side had to wait. Meaningful
//                 once the threads using the channel have finished.
//
// How a channel waits is its own business. MutexQueue sleeps on a condition
// variable. SpinChannel spins on a BoundedQueue.

namespace obe::util {

template <class Q>
concept BoundedQueue = std::constructible_from<Q, std::size_t> && requires {
    typename Q::value_type;
} && requires(Q queue, const Q const_queue, typename Q::value_type value) {
    { queue.try_push(std::move(value)) } -> std::same_as<bool>;
    { queue.try_pop(value) } -> std::same_as<bool>;
    { const_queue.capacity() } -> std::convertible_to<std::size_t>;
};

// How often each side of a channel had to wait. A producer that waits often
// is faster than its consumer, and the reverse; a pipeline's slowest stage is
// the one whose input channel is full and whose output channel is empty.
struct ChannelStats {
    std::size_t push_waits = 0;  // pushes that found the queue full
    std::size_t pop_waits = 0;   // pops that found the queue empty
};

template <class C>
concept Channel = std::constructible_from<C, std::size_t> && requires { typename C::value_type; } &&
                  requires(C channel, const C const_channel, typename C::value_type value) {
                      { channel.push(std::move(value)) } -> std::same_as<bool>;
                      { channel.pop(value) } -> std::same_as<bool>;
                      channel.close();
                      channel.cancel();
                      { const_channel.cancelled() } -> std::same_as<bool>;
                      { const_channel.capacity() } -> std::convertible_to<std::size_t>;
                      { const_channel.stats() } -> std::convertible_to<ChannelStats>;
                  };

}  // namespace obe::util
