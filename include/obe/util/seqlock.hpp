#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "obe/util/backoff.hpp"

// Seqlock<T>: one writer publishes the latest value of T; any number of
// readers take a consistent copy of it, and the writer never waits for them.
//
// What it is for: the newest top-of-book of a security. A reader does not
// want every update in order (that is what the queue is for); it wants to
// know what the book looks like now. And the book thread must never be held
// up by somebody looking.
//
// How it works. Next to the value sits a sequence number.
//
//   writer:  sequence becomes odd      "a write is in progress"
//            write the value
//            sequence becomes even     "done", two higher than before
//
//   reader:  read the sequence; if odd, a write is in progress: try again
//            copy the value
//            read the sequence again; if it changed, the copy may be a mix of
//            two values: throw it away and try again
//
// The writer does a fixed amount of work whatever the readers do: it is
// wait-free. A reader can be made to retry for as long as the writer keeps
// writing, so readers are only lock-free, and a reader that needs a bound has
// to give up after some number of attempts. try_load() is for that.
//
// Why the value is stored as atomics. The textbook version keeps a plain T
// and lets readers copy it while the writer may be changing it, then discards
// torn copies. In C++ that copy is a data race, which is undefined behaviour
// even if the result is thrown away, and ThreadSanitizer reports it. Storing
// the value as 64-bit atomic words makes every access a defined one. On x86
// the generated code is the same plain moves either way.
//
// The memory orderings follow Hans Boehm, "Can seqlocks get along with
// programming language memory models?" (2012):
//
//   - The writer's value stores are release stores and the reader's value
//     loads are acquire loads. A reader that picks up even one word of a
//     write in progress is thereby guaranteed to see everything the writer
//     did before storing that word, including the odd sequence number. Its
//     second read of the sequence then differs from the first, and it retries.
//   - The writer's final sequence store is a release store and the reader's
//     first sequence load an acquire load: a reader that sees the new even
//     number sees the whole value that goes with it.
//
// The paper's faster variant reads the words relaxed and then issues one
// acquire fence. This uses acquire loads instead because ThreadSanitizer does
// not model fences (GCC warns about exactly that). On x86 both compile to the
// same plain moves.
//
// Limits: exactly one writer (two would need a lock between them), and T must
// be trivially copyable, because it is moved around as raw bytes.

namespace obe::util {

template <class T>
    requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class Seqlock {
 public:
    Seqlock() noexcept { store(T{}); }
    explicit Seqlock(const T& initial) noexcept { store(initial); }

    Seqlock(const Seqlock&) = delete;
    Seqlock& operator=(const Seqlock&) = delete;

    // Writer thread only.
    void store(const T& value) noexcept {
        Words raw{};
        std::memcpy(raw.data(), &value, sizeof(T));
        const std::uint64_t seq = seq_.load(std::memory_order_relaxed);
        seq_.store(seq + 1, std::memory_order_relaxed);
        for (std::size_t i = 0; i < kWords; ++i) {
            words_[i].store(raw[i], std::memory_order_release);
        }
        seq_.store(seq + 2, std::memory_order_release);
    }

    // Any thread. Returns a value some store() published in full. Retries for
    // as long as writes keep landing in the middle of its copy.
    [[nodiscard]] T load() const noexcept {
        T out{};
        Backoff backoff;
        while (!try_load(out)) {
            backoff.pause();
        }
        return out;
    }

    // One attempt. Returns false, leaving out alone, if a write was in
    // progress or completed while the value was being copied.
    [[nodiscard]] bool try_load(T& out) const noexcept {
        const std::uint64_t before = seq_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            return false;
        }
        Words raw;
        for (std::size_t i = 0; i < kWords; ++i) {
            raw[i] = words_[i].load(std::memory_order_acquire);
        }
        if (seq_.load(std::memory_order_relaxed) != before) {
            return false;
        }
        // Through void*: T is trivially copyable, which is all memcpy needs, but a
        // T with default member initializers makes the compiler ask for the cast.
        std::memcpy(static_cast<void*>(&out), raw.data(), sizeof(T));
        return true;
    }

    // How many values have been published, counting the initial one. Two
    // loads that return the same number bracket a period with no writes.
    [[nodiscard]] std::uint64_t version() const noexcept {
        return seq_.load(std::memory_order_acquire) / 2;
    }

 private:
    static constexpr std::size_t kWords = (sizeof(T) + sizeof(std::uint64_t) - 1) / 8;
    using Words = std::array<std::uint64_t, kWords>;

    std::atomic<std::uint64_t> seq_{0};
    std::array<std::atomic<std::uint64_t>, kWords> words_{};
};

}  // namespace obe::util
