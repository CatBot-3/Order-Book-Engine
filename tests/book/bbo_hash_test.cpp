#include "obe/book/bbo_hash.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>

#include "obe/book/types.hpp"

namespace {

using namespace obe;
using book::Bbo;
using book::BboHasher;
using book::BboUpdate;

constexpr BboUpdate kBase{.locate = 7, .timestamp = 1'000, .bbo = Bbo{100, 10, 101, 20}};

std::uint64_t hash_of(const BboUpdate& u) {
    BboHasher h;
    h.on_bbo(u);
    return h.hash(u.locate);
}

TEST(BboHasher, StartsEqualForEverySecurity) {
    const BboHasher h;
    EXPECT_EQ(h.hash(0), h.hash(65'535));
    EXPECT_EQ(h.events(7), 0U);
    EXPECT_EQ(h.total_events(), 0U);
    EXPECT_EQ(h.combined(), BboHasher{}.combined());
}

TEST(BboHasher, EveryFieldOfAnUpdateChangesTheHash) {
    std::set<std::uint64_t> seen{hash_of(kBase)};
    BboUpdate u = kBase;
    u.timestamp += 1;
    EXPECT_TRUE(seen.insert(hash_of(u)).second) << "timestamp";
    u = kBase;
    u.bbo.bid_price += 1;
    EXPECT_TRUE(seen.insert(hash_of(u)).second) << "bid price";
    u = kBase;
    u.bbo.bid_qty += 1;
    EXPECT_TRUE(seen.insert(hash_of(u)).second) << "bid size";
    u = kBase;
    u.bbo.ask_price += 1;
    EXPECT_TRUE(seen.insert(hash_of(u)).second) << "ask price";
    u = kBase;
    u.bbo.ask_qty += 1;
    EXPECT_TRUE(seen.insert(hash_of(u)).second) << "ask size";
    // Swapping two fields must not cancel out.
    u = kBase;
    u.bbo = Bbo{101, 20, 100, 10};
    EXPECT_TRUE(seen.insert(hash_of(u)).second) << "bid and ask swapped";
}

TEST(BboHasher, OrderOfUpdatesMatters) {
    BboUpdate second = kBase;
    second.timestamp = 2'000;
    second.bbo.bid_qty = 99;

    BboHasher forward;
    forward.on_bbo(kBase);
    forward.on_bbo(second);
    BboHasher backward;
    backward.on_bbo(second);
    backward.on_bbo(kBase);
    EXPECT_NE(forward.hash(7), backward.hash(7));
    EXPECT_NE(forward.combined(), backward.combined());
    EXPECT_EQ(forward.events(7), 2U);
}

TEST(BboHasher, SecuritiesAreHashedSeparately) {
    BboUpdate other = kBase;
    other.locate = 8;

    BboHasher h;
    h.on_bbo(kBase);
    const std::uint64_t seven = h.hash(7);
    h.on_bbo(other);
    EXPECT_EQ(h.hash(7), seven) << "an update for locate 8 must not disturb locate 7";
    EXPECT_EQ(h.hash(8), seven) << "the same updates give the same per-security hash";
    EXPECT_EQ(h.total_events(), 2U);

    // The combined hash still tells the two securities apart.
    BboHasher only_seven;
    only_seven.on_bbo(kBase);
    only_seven.on_bbo(kBase);
    EXPECT_NE(h.combined(), only_seven.combined());
}

TEST(BboHasher, InterleavingAcrossSecuritiesDoesNotChangeTheResult) {
    // Two securities' updates can arrive in any relative order without
    // changing either per-security hash or the combined hash. This is what
    // lets a threaded pipeline (phase 6) be compared with the single-threaded
    // replay.
    BboUpdate a1 = kBase;
    BboUpdate a2 = kBase;
    a2.timestamp = 5'000;
    BboUpdate b1 = kBase;
    b1.locate = 9;

    BboHasher x;
    x.on_bbo(a1);
    x.on_bbo(a2);
    x.on_bbo(b1);
    BboHasher y;
    y.on_bbo(b1);
    y.on_bbo(a1);
    y.on_bbo(a2);
    EXPECT_EQ(x.combined(), y.combined());
}

}  // namespace
