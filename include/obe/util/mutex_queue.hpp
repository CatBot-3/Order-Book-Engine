#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include "obe/util/queue_concepts.hpp"

// A bounded queue guarded by one mutex, with two condition variables to wait
// on. This is the baseline the lock-free ring has to beat (spec, phase 6).
//
// It is written the way most code bases write it, and nothing about it is
// wrong. What it costs:
//
//   - Every push and every pop takes the mutex. Uncontended, that is an atomic
//     exchange on the way in and another on the way out. Contended, the loser
//     goes to sleep in the kernel and is woken later.
//   - The producer and the consumer write the same few words (the lock, the
//     head, the count), so the cache line holding them moves between their
//     cores on every operation.
//   - A thread that finds the queue empty or full sleeps. Waking it is a
//     system call for the other side and tens of microseconds of latency for
//     the sleeper, which is long compared with handling one message.
//
// It satisfies both contracts in queue_concepts.hpp: the try_ operations never
// wait, and push and pop sleep. It is safe for any number of producers and
// consumers, which is more than the pipeline needs and part of why it is
// slower.

namespace obe::util {

template <class T>
class MutexQueue {
 public:
    using value_type = T;

    // Holds exactly `capacity` items (at least 1).
    explicit MutexQueue(std::size_t capacity) : slots_(capacity == 0 ? 1 : capacity) {}

    MutexQueue(const MutexQueue&) = delete;
    MutexQueue& operator=(const MutexQueue&) = delete;

    // --- BoundedQueue: never waits -------------------------------------------

    template <class U>
    [[nodiscard]] bool try_push(U&& value) {
        {
            const std::lock_guard lock(mutex_);
            if (count_ == slots_.size()) {
                return false;
            }
            put(std::forward<U>(value));
        }
        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) {
        {
            const std::lock_guard lock(mutex_);
            if (count_ == 0) {
                return false;
            }
            take(out);
        }
        not_full_.notify_one();
        return true;
    }

    // --- Channel: sleeps -----------------------------------------------------

    template <class U>
    bool push(U&& value) {
        {
            std::unique_lock lock(mutex_);
            if (count_ == slots_.size() && !cancelled_) {
                ++stats_.push_waits;
                not_full_.wait(lock, [this] { return count_ != slots_.size() || cancelled_; });
            }
            if (cancelled_) {
                return false;
            }
            put(std::forward<U>(value));
        }
        not_empty_.notify_one();
        return true;
    }

    bool pop(T& out) {
        {
            std::unique_lock lock(mutex_);
            if (count_ == 0 && !closed_ && !cancelled_) {
                ++stats_.pop_waits;
                not_empty_.wait(lock, [this] { return count_ != 0 || closed_ || cancelled_; });
            }
            if (cancelled_ || count_ == 0) {
                return false;  // cancelled, or closed and drained
            }
            take(out);
        }
        not_full_.notify_one();
        return true;
    }

    void close() {
        {
            const std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    void cancel() {
        {
            const std::lock_guard lock(mutex_);
            cancelled_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] bool cancelled() const {
        const std::lock_guard lock(mutex_);
        return cancelled_;
    }

    // --- Queries -------------------------------------------------------------

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return count_;
    }

    // Read it after the threads using the queue have finished.
    [[nodiscard]] ChannelStats stats() const {
        const std::lock_guard lock(mutex_);
        return stats_;
    }

 private:
    // Both are called with the mutex held and the precondition checked.
    template <class U>
    void put(U&& value) {
        slots_[(head_ + count_) % slots_.size()] = std::forward<U>(value);
        ++count_;
    }

    void take(T& out) {
        out = std::move(slots_[head_]);
        head_ = (head_ + 1) % slots_.size();
        --count_;
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::vector<T> slots_;
    std::size_t head_ = 0;   // index of the oldest item
    std::size_t count_ = 0;  // items in the queue
    bool closed_ = false;
    bool cancelled_ = false;
    ChannelStats stats_{};
};

}  // namespace obe::util
