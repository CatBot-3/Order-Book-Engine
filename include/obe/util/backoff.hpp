#pragma once

#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

// What a thread does while it waits for another thread without sleeping.

namespace obe::util {

// Tells the CPU this is a spin-wait loop. On x86 that is the PAUSE
// instruction: it costs a few tens of cycles, saves power, gives the other
// hyper-thread of the core a turn, and avoids the pipeline flush a tight loop
// suffers when the awaited memory finally changes.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

// Spin for a while, then start giving the time slice away.
//
// Pure spinning is the fastest way to wait when the other thread has a core
// of its own: there is no system call and no wake-up delay. It is the worst
// way when it does not: the waiter burns the very time slice the other thread
// needs to make progress. That happens on a machine with fewer free cores
// than threads, in CI, and under ThreadSanitizer. Yielding after a bounded
// number of spins keeps those cases alive at a small cost to the fast one.
class Backoff {
 public:
    void pause() noexcept {
        if (spins_ < kSpinLimit) {
            ++spins_;
            cpu_relax();
        } else {
            std::this_thread::yield();
        }
    }

    void reset() noexcept { spins_ = 0; }

 private:
    static constexpr unsigned kSpinLimit = 256;
    unsigned spins_ = 0;
};

}  // namespace obe::util
