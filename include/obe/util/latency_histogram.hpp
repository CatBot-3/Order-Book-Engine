#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// A fixed-size histogram for latency samples.
//
// Requirements it is built to (spec phase 3): fixed buckets, no allocation
// while recording, percentiles on demand.
//
// Buckets are log-linear, the scheme HdrHistogram made familiar. Each power of
// two is split into 32 equal sub-buckets, so a bucket is never wider than 1/32
// of the values it holds: about 3% relative error at any magnitude. Values
// below 64 get a bucket each and are exact. That fits latencies well: 40 ns
// and 41 ns stay distinct, while 4.00 ms and 4.01 ms need not.
//
// The whole table is 1152 counters (about 9 KB) and covers values up to 2^40,
// which is 18 minutes in nanoseconds. Recording is a count-leading-zeros, a
// shift and an increment.
//
// The unit is whatever the caller records. replay_bench records raw clock
// ticks and converts percentiles to nanoseconds when it reports, so there is
// no floating-point work per sample.

namespace obe::util {

class LatencyHistogram {
 public:
    static constexpr unsigned kSubBucketBits = 5;
    static constexpr std::uint64_t kSubBuckets = std::uint64_t{1} << kSubBucketBits;  // 32
    static constexpr unsigned kMaxBits = 40;
    static constexpr std::uint64_t kMaxTrackable = (std::uint64_t{1} << kMaxBits) - 1;
    static constexpr std::size_t kBuckets = (kMaxBits - kSubBucketBits) * kSubBuckets + kSubBuckets;

    // The bucket a value lands in. Values above kMaxTrackable share the last one.
    [[nodiscard]] static constexpr std::size_t bucket_index(std::uint64_t v) noexcept {
        if (v < kSubBuckets) {
            return static_cast<std::size_t>(v);
        }
        if (v > kMaxTrackable) {
            v = kMaxTrackable;
        }
        // msb is the position of the highest set bit, at least kSubBucketBits.
        // Keep the top kSubBucketBits + 1 bits of v: the leading one and the
        // five bits below it.
        const unsigned msb = 63U - static_cast<unsigned>(std::countl_zero(v));
        const unsigned shift = msb - kSubBucketBits;
        return static_cast<std::size_t>(shift * kSubBuckets + (v >> shift));
    }

    // The smallest and largest values that land in bucket i.
    [[nodiscard]] static constexpr std::uint64_t bucket_low(std::size_t i) noexcept {
        if (i < 2 * kSubBuckets) {
            return i;
        }
        const std::uint64_t shift = i / kSubBuckets - 1;
        return (kSubBuckets + i % kSubBuckets) << shift;
    }
    [[nodiscard]] static constexpr std::uint64_t bucket_high(std::size_t i) noexcept {
        if (i < 2 * kSubBuckets) {
            return i;
        }
        const std::uint64_t shift = i / kSubBuckets - 1;
        return bucket_low(i) + (std::uint64_t{1} << shift) - 1;
    }

    void record(std::uint64_t v) noexcept {
        ++counts_[bucket_index(v)];
        ++count_;
        sum_ += v;
        min_ = v < min_ ? v : min_;
        max_ = v > max_ ? v : max_;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

    // Exact, not bucketed. Both are 0 for an empty histogram.
    [[nodiscard]] std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }

    // Exact mean of the recorded values. 0 for an empty histogram.
    [[nodiscard]] double mean() const noexcept {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    // The value at or below which `percent` percent of samples fall.
    //
    // It is the upper edge of the bucket holding that sample, capped at the
    // largest value recorded, so it never understates a latency and is at most
    // one bucket width (3%) above the true value. percentile(100) is the exact
    // maximum. Returns 0 for an empty histogram.
    [[nodiscard]] std::uint64_t percentile(double percent) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        const double clamped = std::clamp(percent, 0.0, 100.0);
        auto rank =
            static_cast<std::uint64_t>(std::ceil(clamped / 100.0 * static_cast<double>(count_)));
        rank = std::clamp<std::uint64_t>(rank, 1, count_);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank) {
                return std::clamp(bucket_high(i), min_, max_);
            }
        }
        return max_;
    }

    // Add another histogram's samples to this one.
    void merge(const LatencyHistogram& other) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) {
            counts_[i] += other.counts_[i];
        }
        count_ += other.count_;
        sum_ += other.sum_;
        min_ = other.min_ < min_ ? other.min_ : min_;
        max_ = other.max_ > max_ ? other.max_ : max_;
    }

    void reset() noexcept { *this = LatencyHistogram{}; }

 private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_ = 0;
};

}  // namespace obe::util
