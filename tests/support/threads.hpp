#pragma once

#include <atomic>
#include <exception>
#include <thread>
#include <utility>

// Helpers for tests that start threads.
//
// Two things go wrong when a test thread fails and nobody planned for it. An
// exception that escapes a thread ends the whole process, taking every other
// test's result with it. And the thread on the other end of a queue waits
// for ever for an item that is not coming. A queue that is not written yet
// does both at once, so these tests are built to fail cleanly:
//
//   - Worker runs its body on a thread, catches whatever it throws, and
//     rethrows it from join() on the test's own thread.
//   - Abort is a flag every waiting loop checks. A Worker raises it when its
//     body throws, and a StopOnExit raises it when the test's own side leaves
//     its scope for any reason.

namespace obe::test {

struct Abort {
    std::atomic<bool> flag{false};

    void raise() noexcept { flag.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool raised() const noexcept { return flag.load(std::memory_order_relaxed); }
};

class Worker {
 public:
    template <class F>
    Worker(Abort& abort, F body)
        : thread_([this, &abort, body = std::move(body)]() mutable {
              try {
                  body();
              } catch (...) {
                  error_ = std::current_exception();
                  abort.raise();
              }
          }) {}

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    ~Worker() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Waits for the thread and rethrows what its body threw, if anything.
    void join() {
        thread_.join();
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

 private:
    std::exception_ptr error_;
    std::thread thread_;
};

// Raises the flag when it goes out of scope. Declare it after the Workers, so
// that it is destroyed, and the flag raised, before their destructors join.
class StopOnExit {
 public:
    explicit StopOnExit(Abort& abort) noexcept : abort_(&abort) {}
    StopOnExit(const StopOnExit&) = delete;
    StopOnExit& operator=(const StopOnExit&) = delete;
    ~StopOnExit() { abort_->raise(); }

 private:
    Abort* abort_;
};

}  // namespace obe::test
