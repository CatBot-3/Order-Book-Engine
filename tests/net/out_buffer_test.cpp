#include "obe/net/out_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "obe/gen/rng.hpp"
#include "support/tcp_client.hpp"

// The buffer a readiness transport keeps a connection's unsent output in
// (obe/net/out_buffer.hpp). No sockets here: the test plays the kernel, which
// takes as much of what it is offered as it pleases.

namespace {

using namespace obe;
using net::OutBuffer;
using test::pattern;

using Bytes = std::vector<std::byte>;

void append(Bytes& to, std::span<const std::byte> more) {
    to.insert(to.end(), more.begin(), more.end());
}

// Takes up to `most` bytes from the front of the buffer, as a send() would,
// and adds them to `wire`. Returns how many.
std::size_t take(OutBuffer& out, std::size_t most, Bytes& wire) {
    const std::span<const std::byte> offered = out.pending();
    const std::size_t count = std::min(most, offered.size());
    append(wire, offered.first(count));
    out.took(count);
    return count;
}

TEST(OutBuffer, StartsEmpty) {
    const OutBuffer out;
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(out.unsent(), 0U);
    EXPECT_EQ(out.held(), 0U);
    EXPECT_TRUE(out.pending().empty());
}

TEST(OutBuffer, OffersWhatWasAppendedInOrder) {
    OutBuffer out;
    const Bytes first = pattern(1, 300);
    const Bytes second = pattern(2, 500);
    out.append(first);
    out.append(second);
    EXPECT_FALSE(out.empty());
    EXPECT_EQ(out.unsent(), 800U);
    Bytes expected = first;
    append(expected, second);
    const std::span<const std::byte> offered = out.pending();
    EXPECT_TRUE(std::equal(offered.begin(), offered.end(), expected.begin(), expected.end()));
}

TEST(OutBuffer, CarriesOnFromWhereTheKernelStopped) {
    OutBuffer out;
    const Bytes all = pattern(3, 1'000);
    out.append(all);
    Bytes wire;
    EXPECT_EQ(take(out, 300, wire), 300U);
    EXPECT_EQ(out.unsent(), 700U);
    EXPECT_FALSE(out.empty());
    EXPECT_EQ(take(out, 300, wire), 300U);
    EXPECT_EQ(out.unsent(), 400U);
    EXPECT_EQ(take(out, 1'000, wire), 400U);
    EXPECT_EQ(wire, all);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(out.unsent(), 0U);
}

TEST(OutBuffer, HoldsNothingOnceEverythingIsTaken) {
    OutBuffer out;
    out.append(pattern(4, 10'000));
    Bytes wire;
    take(out, 10'000, wire);
    EXPECT_EQ(out.held(), 0U);
    EXPECT_TRUE(out.pending().empty());
    // And it is as good as new.
    const Bytes more = pattern(5, 77);
    out.append(more);
    wire.clear();
    take(out, 77, wire);
    EXPECT_EQ(wire, more);
}

TEST(OutBuffer, TakingNothingChangesNothing) {
    OutBuffer out;
    const Bytes all = pattern(6, 200);
    out.append(all);
    out.took(0);
    EXPECT_EQ(out.unsent(), 200U);
    Bytes wire;
    take(out, 200, wire);
    EXPECT_EQ(wire, all);
}

// The hole this buffer exists to close. The client reads steadily and is
// written to just as steadily, so there is always a little left: the buffer
// is never empty and the backlog never grows. What it keeps in memory must
// stay in proportion to that backlog, and not to everything it ever carried.
TEST(OutBuffer, ABacklogThatNeverDrainsDoesNotKeepWhatWasSent) {
    OutBuffer out;
    Bytes queued;
    Bytes wire;
    const Bytes backlog = pattern(7, 5'000);
    out.append(backlog);
    append(queued, backlog);
    std::size_t most_held = 0;
    for (std::uint64_t round = 0; round < 2'000; ++round) {
        const Bytes more = pattern(100 + round, 1'000);
        out.append(more);
        append(queued, more);
        ASSERT_EQ(take(out, 1'000, wire), 1'000U);
        ASSERT_EQ(out.unsent(), 5'000U) << "the backlog is the same after every round";
        ASSERT_FALSE(out.empty());
        most_held = std::max(most_held, out.held());
    }
    // Two million bytes went through. Twice the backlog is the bound.
    EXPECT_LE(most_held, 2U * 5'000U);
    take(out, 5'000, wire);
    EXPECT_EQ(wire, queued) << "and every byte came out, once, in order";
}

// Any mixture of appending and taking: the bytes that come out are the bytes
// that went in, and the memory held never passes twice what is unsent.
TEST(OutBuffer, AnyMixtureOfAppendingAndTakingKeepsOrderAndTheBound) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        gen::SplitMix64 rng(seed);
        OutBuffer out;
        Bytes queued;
        Bytes wire;
        for (int step = 0; step < 3'000; ++step) {
            if (rng.below(3) != 0) {
                const Bytes more =
                    pattern(seed * 10'000 + static_cast<std::uint64_t>(step), rng.below(600));
                out.append(more);
                append(queued, more);
            } else {
                // Sometimes a few bytes, sometimes everything and more.
                const std::size_t most = rng.below(4) == 0 ? 100'000 : rng.below(900);
                take(out, most, wire);
                ASSERT_LE(out.held(), 2 * out.unsent());
            }
            ASSERT_EQ(out.unsent(), queued.size() - wire.size());
            ASSERT_EQ(out.empty(), queued.size() == wire.size());
            ASSERT_GE(out.held(), out.unsent());
        }
        take(out, queued.size(), wire);
        ASSERT_TRUE(wire == queued);
        ASSERT_EQ(out.held(), 0U);
    }
}

}  // namespace
