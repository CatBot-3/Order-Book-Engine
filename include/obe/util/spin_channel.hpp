#pragma once

#include <atomic>
#include <cstddef>
#include <utility>

#include "obe/util/backoff.hpp"
#include "obe/util/cache_line.hpp"
#include "obe/util/queue_concepts.hpp"

// Turns a queue that never waits into a channel that waits by spinning.
//
// A lock-free ring only offers try_push and try_pop. A pipeline stage needs
// more: "give me the next item, however long it takes, and tell me when there
// will be no more". This adds exactly that and nothing else, so the ring
// itself can stay as small as the structure it is.
//
// Waiting here means retrying. No thread ever sleeps in the kernel, so nobody
// ever has to be woken, and an item is picked up within nanoseconds of being
// pushed. The price is a core that runs at full power while it has nothing to
// do. That is the usual trade in a trading system and the wrong one almost
// everywhere else.
//
// Wrapping MutexQueue in this gives a third thing to measure: a locked queue
// that spins instead of sleeping. Comparing all three separates the cost of
// the lock from the cost of the sleep, which a two-way comparison cannot.

namespace obe::util {

// (The static analyser reports the padding in this class as excessive. It is
// deliberate: the two wait counters and the closed flag are kept on separate
// cache lines, as explained at the members.)
template <BoundedQueue Queue>
class SpinChannel {  // NOLINT(clang-analyzer-optin.performance.Padding)
 public:
    using value_type = typename Queue::value_type;

    explicit SpinChannel(std::size_t capacity) : queue_(capacity) {}

    SpinChannel(const SpinChannel&) = delete;
    SpinChannel& operator=(const SpinChannel&) = delete;

    // Producer thread only.
    template <class U>
    bool push(U&& value) {
        if (cancelled_.load(std::memory_order_relaxed)) [[unlikely]] {
            return false;
        }
        // try_push leaves the value alone when it fails, so forwarding it
        // again on the next attempt is sound.
        if (queue_.try_push(std::forward<U>(value))) {
            return true;
        }
        ++producer_.waits;
        Backoff backoff;
        while (!queue_.try_push(std::forward<U>(value))) {
            if (cancelled_.load(std::memory_order_relaxed)) {
                return false;
            }
            backoff.pause();
        }
        return true;
    }

    // Producer thread only. Call it after the last push.
    void close() noexcept { closed_.store(true, std::memory_order_release); }

    // Consumer thread only.
    bool pop(value_type& out) {
        // Cancelled means "stop now", even if items are still queued. The flag
        // is written at most once in a channel's life, so reading it on every
        // operation costs a load from a cache line nobody is writing.
        if (cancelled_.load(std::memory_order_relaxed)) [[unlikely]] {
            return false;
        }
        if (queue_.try_pop(out)) {
            return true;
        }
        ++consumer_.waits;
        Backoff backoff;
        for (;;) {
            if (cancelled_.load(std::memory_order_relaxed)) {
                return false;
            }
            // Read "closed" before looking at the queue, not after. The
            // producer pushes its last item and then closes. If the queue is
            // found empty after closed was seen true, it is empty for good.
            // The other order has a hole: the queue looks empty, the producer
            // pushes its last item and closes, closed is then seen true, and
            // that last item would be lost.
            const bool closed = closed_.load(std::memory_order_acquire);
            if (queue_.try_pop(out)) {
                return true;
            }
            if (closed) {
                return false;
            }
            backoff.pause();
        }
    }

    // Any thread.
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool cancelled() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t capacity() const { return queue_.capacity(); }

    // Read it after the threads using the channel have finished: each counter
    // is written by one side without synchronization.
    [[nodiscard]] ChannelStats stats() const noexcept { return {producer_.waits, consumer_.waits}; }

 private:
    // Each side counts its own waits, on a line of its own so that counting
    // does not create the sharing the ring was built to avoid.
    struct alignas(kCacheLine) Side {
        std::size_t waits = 0;
    };

    Queue queue_;
    Side producer_;
    Side consumer_;
    alignas(kCacheLine) std::atomic<bool> closed_{false};
    std::atomic<bool> cancelled_{false};
};

}  // namespace obe::util
