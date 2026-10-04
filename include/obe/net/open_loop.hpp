#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

// The two small pieces of arithmetic that make load_gen's latencies honest.
//
// "Open loop" means the load is decided by a schedule, not by the server. A
// closed-loop generator sends a request, waits for the answer, and sends the
// next: when the server slows down, so does the load, and the slow period is
// under-represented in the generator's own samples. An open-loop generator
// keeps to its schedule whatever the server does, the way real clients do:
// they do not stop wanting to trade because the exchange is having a bad
// moment.
//
// They are separate from the sockets so that the claim can be tested without
// a network and without real time.

namespace obe::net {

// When each request is due: request i at start + i / rate seconds.
class OpenLoopSchedule {
 public:
    // `start_ns` and the results are nanoseconds on whatever clock the caller
    // uses. `per_second` must be at least 1.
    constexpr OpenLoopSchedule(std::uint64_t start_ns, std::uint64_t per_second) noexcept
        : start_(start_ns), rate_(per_second == 0 ? 1 : per_second) {}

    // Computed from the request's number, not by adding an interval to the
    // previous due time: a rounded interval added a million times drifts, and
    // the rate actually offered would not be the rate asked for.
    [[nodiscard]] constexpr std::uint64_t due(std::uint64_t i) const noexcept {
        // Whole seconds and the remainder separately, so that the product
        // cannot overflow 64 bits.
        return start_ + (i / rate_) * kSecond + (i % rate_) * kSecond / rate_;
    }

    [[nodiscard]] constexpr std::uint64_t rate() const noexcept { return rate_; }

 private:
    static constexpr std::uint64_t kSecond = 1'000'000'000ULL;

    std::uint64_t start_;
    std::uint64_t rate_;
};

// The requests one connection is still waiting to hear about, oldest first.
//
// The gateway answers each connection's requests in the order it received
// them. So an acknowledgement needs no token to be matched: it belongs to the
// oldest request not yet answered.
class AckQueue {
 public:
    // A request due at `due_ns` was handed to the connection.
    void sent(std::uint64_t due_ns) { due_.push_back(due_ns); }

    [[nodiscard]] bool empty() const noexcept { return due_.empty(); }
    [[nodiscard]] std::size_t waiting() const noexcept { return due_.size(); }

    // The due time of the request the next acknowledgement will answer.
    // Precondition: !empty().
    [[nodiscard]] std::uint64_t oldest() const noexcept { return due_.front(); }

    // An acknowledgement arrived at `arrived_ns`. Removes the oldest request
    // and returns how long it took, counted from when it was DUE. A request
    // that was written late, because the generator or the socket was behind,
    // is therefore charged for the wait: that wait is real for whoever wanted
    // the order placed at the due time.
    // Precondition: !empty().
    std::uint64_t acknowledged(std::uint64_t arrived_ns) {
        const std::uint64_t due = due_.front();
        due_.pop_front();
        return arrived_ns > due ? arrived_ns - due : 0;
    }

 private:
    std::deque<std::uint64_t> due_;
};

}  // namespace obe::net
