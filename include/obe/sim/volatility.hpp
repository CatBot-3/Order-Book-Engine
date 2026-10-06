#pragma once

#include <cmath>

#include "obe/sim/types.hpp"
#include "obe/types.hpp"

// How much the mid has been moving lately, as a variance per second.
//
// The mid is observed at irregular times: whenever the best bid or offer
// changes. If it moved like a random walk with variance sigma^2 per second,
// the expected square of a move over dt seconds would be sigma^2 * dt. So
//
//     sigma^2  is estimated by  sum of (move)^2 / sum of dt
//
// over the moves observed. Dividing sums, and not averaging (move)^2 / dt
// move by move, matters: two observations a microsecond apart with a one-tick
// move between them would otherwise contribute an enormous value by
// themselves.
//
// Both sums are faded with age, with the given half-life, so the estimate
// follows the market: quiet at lunch, busy at the open and the close.
//
// Floating point is right here. This is a statistic that feeds a pricing
// formula; no money is counted in it.

namespace obe::sim {

class EwmaVariance {
 public:
    // `half_life`: how old an observation is when its weight has halved. Zero
    // means observations never fade, which gives the plain estimate over
    // everything seen.
    explicit EwmaVariance(Nanos half_life = 60 * kNanosPerSecond) noexcept
        : decay_per_second_(
              half_life == 0 ? 0.0 : std::log(2.0) / (static_cast<double>(half_life) / 1e9)) {}

    // The mid, in dollars, at time `now`. Observations must come in time
    // order; one that is not later than the last is taken to be at the same
    // instant as it.
    void observe(Nanos now, double mid) noexcept {
        if (!seen_) {
            seen_ = true;
            last_time_ = now;
            last_mid_ = mid;
            return;
        }
        if (now > last_time_) {
            const double dt = static_cast<double>(now - last_time_) / 1e9;
            const double fade = std::exp(-decay_per_second_ * dt);
            const double move = mid - last_mid_;
            squares_ = squares_ * fade + move * move;
            seconds_ = seconds_ * fade + dt;
            last_time_ = now;
        } else {
            // Several changes with one timestamp: the move is counted, and no
            // time has passed for it to be spread over.
            const double move = mid - last_mid_;
            squares_ += move * move;
        }
        last_mid_ = mid;
    }

    // Dollars squared per second. Zero until time has passed between two
    // observations.
    [[nodiscard]] double value() const noexcept {
        return seconds_ > 0.0 ? squares_ / seconds_ : 0.0;
    }

    [[nodiscard]] bool ready() const noexcept { return seconds_ > 0.0; }

 private:
    double decay_per_second_;
    bool seen_ = false;
    Nanos last_time_ = 0;
    double last_mid_ = 0;
    double squares_ = 0;  // faded sum of squared moves
    double seconds_ = 0;  // faded sum of the time they took
};

}  // namespace obe::sim
