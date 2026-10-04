#include "obe/util/latency_histogram.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "obe/gen/synthetic_feed.hpp"

namespace {

using obe::util::LatencyHistogram;

// "No allocation while recording" is a property of the type: a fixed-size
// object with no pointers in it cannot allocate.
static_assert(std::is_trivially_copyable_v<LatencyHistogram>);
static_assert(sizeof(LatencyHistogram) < std::size_t{16} * 1024);
static_assert(LatencyHistogram::kBuckets == 1152);

TEST(LatencyHistogram, BucketsTileTheRangeWithNoGapsOrOverlaps) {
    EXPECT_EQ(LatencyHistogram::bucket_low(0), 0U);
    for (std::size_t i = 0; i + 1 < LatencyHistogram::kBuckets; ++i) {
        ASSERT_LE(LatencyHistogram::bucket_low(i), LatencyHistogram::bucket_high(i)) << i;
        ASSERT_EQ(LatencyHistogram::bucket_high(i) + 1, LatencyHistogram::bucket_low(i + 1)) << i;
    }
    EXPECT_EQ(LatencyHistogram::bucket_high(LatencyHistogram::kBuckets - 1),
              LatencyHistogram::kMaxTrackable);
}

TEST(LatencyHistogram, EveryValueLandsInTheBucketThatContainsIt) {
    const auto check = [](std::uint64_t v) {
        const std::size_t i = LatencyHistogram::bucket_index(v);
        ASSERT_LT(i, LatencyHistogram::kBuckets) << v;
        ASSERT_LE(LatencyHistogram::bucket_low(i), v) << v;
        ASSERT_GE(LatencyHistogram::bucket_high(i), v) << v;
    };
    for (std::uint64_t v = 0; v < 200'000; ++v) {
        check(v);
    }
    // Around every power of two, where an off-by-one would hide.
    for (unsigned bit = 5; bit < LatencyHistogram::kMaxBits; ++bit) {
        const std::uint64_t p = std::uint64_t{1} << bit;
        for (const std::uint64_t v : {p - 1, p, p + 1, p + (p >> 1), 2 * p - 1}) {
            check(v);
        }
    }
    check(LatencyHistogram::kMaxTrackable);
}

TEST(LatencyHistogram, SmallValuesAreExactAndLargeOnesWithinThreePercent) {
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(LatencyHistogram::bucket_low(i), LatencyHistogram::bucket_high(i)) << i;
    }
    for (std::size_t i = 64; i < LatencyHistogram::kBuckets; ++i) {
        const std::uint64_t low = LatencyHistogram::bucket_low(i);
        const std::uint64_t width = LatencyHistogram::bucket_high(i) - low + 1;
        ASSERT_LE(width * 32, low) << "bucket " << i << " is wider than 1/32 of its lower edge";
    }
}

TEST(LatencyHistogram, ValuesBeyondTheRangeSaturateButTheExactMaxIsKept) {
    LatencyHistogram h;
    const std::uint64_t huge = std::uint64_t{1} << 50;
    h.record(huge);
    EXPECT_EQ(LatencyHistogram::bucket_index(huge), LatencyHistogram::kBuckets - 1);
    EXPECT_EQ(h.max(), huge);
    EXPECT_EQ(h.percentile(100), huge);
    EXPECT_EQ(h.count(), 1U);
}

TEST(LatencyHistogram, EmptyHistogramReportsZeros) {
    const LatencyHistogram h;
    EXPECT_TRUE(h.empty());
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.min(), 0U);
    EXPECT_EQ(h.max(), 0U);
    EXPECT_EQ(h.mean(), 0.0);
    EXPECT_EQ(h.percentile(50), 0U);
    EXPECT_EQ(h.percentile(100), 0U);
}

TEST(LatencyHistogram, CountMinMaxAndMeanAreExact) {
    LatencyHistogram h;
    for (const std::uint64_t v : {10U, 20U, 30U, 1'000'000U}) {
        h.record(v);
    }
    EXPECT_FALSE(h.empty());
    EXPECT_EQ(h.count(), 4U);
    EXPECT_EQ(h.min(), 10U);
    EXPECT_EQ(h.max(), 1'000'000U);
    EXPECT_DOUBLE_EQ(h.mean(), (10.0 + 20.0 + 30.0 + 1'000'000.0) / 4.0);
}

TEST(LatencyHistogram, PercentilesOfOneToSixtyAreExact) {
    LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 60; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.percentile(50), 30U);
    EXPECT_EQ(h.percentile(90), 54U);
    EXPECT_EQ(h.percentile(100), 60U);
    EXPECT_EQ(h.percentile(0), 1U) << "the 0th percentile is the smallest sample";
    EXPECT_EQ(h.percentile(1), 1U);
}

TEST(LatencyHistogram, ASingleOutlierShowsInTheTailNotTheMedian) {
    LatencyHistogram h;
    for (int i = 0; i < 9'999; ++i) {
        h.record(40);
    }
    h.record(5'000'000);
    EXPECT_EQ(h.percentile(50), 40U);
    EXPECT_EQ(h.percentile(99), 40U);
    EXPECT_EQ(h.percentile(99.9), 40U);
    EXPECT_EQ(h.percentile(100), 5'000'000U);
    EXPECT_EQ(h.max(), 5'000'000U);
}

// The property that matters for reporting: a percentile is never below the
// true value (it never flatters) and never more than one bucket above it.
TEST(LatencyHistogram, PercentilesBracketTheExactAnswerOnRandomData) {
    obe::gen::SplitMix64 rng(99);
    auto h = std::make_unique<LatencyHistogram>();
    std::vector<std::uint64_t> exact;
    for (int i = 0; i < 200'000; ++i) {
        // A long-tailed shape: mostly tens, sometimes thousands, rarely millions.
        std::uint64_t v = 20 + rng.below(80);
        if (rng.below(50) == 0) {
            v = 1'000 + rng.below(20'000);
        }
        if (rng.below(5'000) == 0) {
            v = 1'000'000 + rng.below(50'000'000);
        }
        h->record(v);
        exact.push_back(v);
    }
    std::sort(exact.begin(), exact.end());
    for (const double p : {1.0, 10.0, 25.0, 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 100.0}) {
        auto rank =
            static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(exact.size())));
        rank = std::clamp<std::size_t>(rank, 1, exact.size());
        const std::uint64_t truth = exact[rank - 1];
        const std::uint64_t got = h->percentile(p);
        EXPECT_GE(got, truth) << "p" << p << " understates";
        EXPECT_LE(got, truth + truth / 32 + 1) << "p" << p << " is more than one bucket high";
    }
    EXPECT_EQ(h->percentile(100), exact.back());
    EXPECT_EQ(h->min(), exact.front());
}

TEST(LatencyHistogram, PercentilesNeverDecreaseAsThePercentRises) {
    obe::gen::SplitMix64 rng(3);
    LatencyHistogram h;
    for (int i = 0; i < 50'000; ++i) {
        h.record(rng.below(1'000'000));
    }
    std::uint64_t previous = 0;
    for (int quarter = 0; quarter <= 400; ++quarter) {
        const double p = quarter * 0.25;
        const std::uint64_t v = h.percentile(p);
        ASSERT_GE(v, previous) << "p" << p;
        previous = v;
    }
}

TEST(LatencyHistogram, OutOfRangePercentsAreClamped) {
    LatencyHistogram h;
    h.record(5);
    h.record(7);
    EXPECT_EQ(h.percentile(-10), 5U);
    EXPECT_EQ(h.percentile(250), 7U);
}

TEST(LatencyHistogram, MergeIsTheSameAsRecordingEverythingInOne) {
    obe::gen::SplitMix64 rng(17);
    LatencyHistogram a;
    LatencyHistogram b;
    LatencyHistogram all;
    for (int i = 0; i < 20'000; ++i) {
        const std::uint64_t v = rng.below(10'000'000);
        (i % 3 == 0 ? a : b).record(v);
        all.record(v);
    }
    a.merge(b);
    EXPECT_EQ(a.count(), all.count());
    EXPECT_EQ(a.min(), all.min());
    EXPECT_EQ(a.max(), all.max());
    EXPECT_DOUBLE_EQ(a.mean(), all.mean());
    for (const double p : {50.0, 99.0, 99.9, 100.0}) {
        EXPECT_EQ(a.percentile(p), all.percentile(p)) << p;
    }

    LatencyHistogram empty;
    a.merge(empty);
    EXPECT_EQ(a.count(), all.count());
    EXPECT_EQ(a.min(), all.min()) << "merging an empty histogram must not disturb the minimum";
}

TEST(LatencyHistogram, ResetReturnsToEmpty) {
    LatencyHistogram h;
    h.record(123);
    h.reset();
    EXPECT_TRUE(h.empty());
    EXPECT_EQ(h.max(), 0U);
    EXPECT_EQ(h.percentile(50), 0U);
    h.record(7);
    EXPECT_EQ(h.min(), 7U);
    EXPECT_EQ(h.max(), 7U);
}

}  // namespace
