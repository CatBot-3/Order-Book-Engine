#include "obe/util/pool.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "obe/gen/synthetic_feed.hpp"
#include "obe/util/pool_allocator.hpp"

// The judge for util::Pool. Each test runs for four element types chosen to
// catch the usual mistakes: one smaller than a pointer, one ordinary, and two
// with stricter alignment than any pointer needs.

namespace {

using obe::util::Pool;
using obe::util::PoolAllocator;

struct Tiny {
    std::uint8_t value;
};

struct Ordinary {
    std::uint64_t a;
    std::uint32_t b;
    std::uint32_t c;
    std::uint64_t d;
};

struct alignas(64) CacheLine {
    std::uint64_t words[8];
};

// Page alignment is here to make an alignment mistake impossible to miss. A
// slab obtained with too little alignment often lands on a 64-byte boundary by
// luck (under AddressSanitizer it nearly always does), but it essentially never
// lands on a 4096-byte one.
struct alignas(4096) PageAligned {
    std::byte bytes[4096];
};

template <class T>
class PoolTest : public ::testing::Test {
 protected:
    // Fill every byte of a slot with a value derived from `tag`, and check it
    // later. Two slots that overlap, or a free-list link written into a slot
    // that is in use, show up as a corrupted pattern.
    static void fill(void* p, std::uint64_t tag) {
        std::memset(p, static_cast<int>(pattern(tag)), sizeof(T));
    }
    static bool intact(const void* p, std::uint64_t tag) {
        const auto* bytes = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            if (bytes[i] != pattern(tag)) {
                return false;
            }
        }
        return true;
    }
    static unsigned char pattern(std::uint64_t tag) {
        return static_cast<unsigned char>(1 + tag % 251);
    }
    static bool aligned(const void* p) {
        return reinterpret_cast<std::size_t>(p) % alignof(T) == 0;
    }
};

using ElementTypes = ::testing::Types<Tiny, Ordinary, CacheLine, PageAligned>;
TYPED_TEST_SUITE(PoolTest, ElementTypes);

TYPED_TEST(PoolTest, StartsEmptyAndAllocatesNothingUntilUsed) {
    const Pool<TypeParam> pool(64);
    EXPECT_EQ(pool.in_use(), 0U);
    EXPECT_EQ(pool.capacity(), 0U) << "the first slab should be allocated on first use";
}

TYPED_TEST(PoolTest, HandsOutDistinctAlignedSlots) {
    Pool<TypeParam> pool(64);
    std::set<void*> seen;
    std::vector<void*> slots;
    for (std::uint64_t i = 0; i < 1'000; ++i) {
        void* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        ASSERT_TRUE(this->aligned(p)) << "slot " << i << " is not aligned for the element type";
        ASSERT_TRUE(seen.insert(p).second) << "slot " << i << " was handed out twice";
        this->fill(p, i);
        slots.push_back(p);
    }
    EXPECT_EQ(pool.in_use(), 1'000U);
    EXPECT_GE(pool.capacity(), 1'000U);
    for (std::uint64_t i = 0; i < slots.size(); ++i) {
        ASSERT_TRUE(this->intact(slots[i], i)) << "slot " << i << " was overwritten by another";
    }
    for (void* p : slots) {
        pool.deallocate(p);
    }
    EXPECT_EQ(pool.in_use(), 0U);
}

TYPED_TEST(PoolTest, GrowingKeepsEarlierSlotsValid) {
    // Slabs of 8, and 500 allocations: dozens of growth steps. A pool built on
    // one reallocating array would have moved the early slots by now.
    Pool<TypeParam> pool(8);
    std::vector<void*> slots;
    for (std::uint64_t i = 0; i < 500; ++i) {
        void* p = pool.allocate();
        this->fill(p, i);
        slots.push_back(p);
        ASSERT_TRUE(this->intact(slots.front(), 0)) << "the first slot changed after " << i;
    }
    for (std::uint64_t i = 0; i < slots.size(); ++i) {
        ASSERT_TRUE(this->intact(slots[i], i)) << "slot " << i;
    }
    for (void* p : slots) {
        pool.deallocate(p);
    }
}

TYPED_TEST(PoolTest, ReusesFreedSlotsInsteadOfGrowing) {
    Pool<TypeParam> pool(32);
    std::vector<void*> slots;
    slots.reserve(100);
    for (int i = 0; i < 100; ++i) {
        slots.push_back(pool.allocate());
    }
    const std::size_t capacity = pool.capacity();

    for (int round = 0; round < 50; ++round) {
        for (void* p : slots) {
            pool.deallocate(p);
        }
        ASSERT_EQ(pool.in_use(), 0U);
        slots.clear();
        std::set<void*> distinct;
        for (int i = 0; i < 100; ++i) {
            void* p = pool.allocate();
            ASSERT_TRUE(distinct.insert(p).second) << "handed out twice in one round";
            slots.push_back(p);
        }
        ASSERT_EQ(pool.in_use(), 100U);
        ASSERT_EQ(pool.capacity(), capacity) << "steady-state churn must not grow the pool";
    }
    for (void* p : slots) {
        pool.deallocate(p);
    }
}

TYPED_TEST(PoolTest, ASlabOfOneStillWorks) {
    Pool<TypeParam> pool(1);
    void* a = pool.allocate();
    void* b = pool.allocate();
    void* c = pool.allocate();
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
    EXPECT_EQ(pool.in_use(), 3U);
    pool.deallocate(b);
    EXPECT_EQ(pool.in_use(), 2U);
    void* d = pool.allocate();
    EXPECT_EQ(d, b) << "the only free slot is the one just returned";
    pool.deallocate(a);
    pool.deallocate(c);
    pool.deallocate(d);
    EXPECT_EQ(pool.in_use(), 0U);
}

// Model-based test: random allocations and frees, with every live slot's
// contents checked whenever it is freed and again at the end.
TYPED_TEST(PoolTest, AgreesWithAModelUnderRandomUse) {
    Pool<TypeParam> pool(128);
    obe::gen::SplitMix64 rng(0x9001);
    std::vector<std::pair<void*, std::uint64_t>> live;  // slot and the tag it was filled with
    std::uint64_t next_tag = 0;
    for (int step = 0; step < 200'000; ++step) {
        const bool allocate = live.empty() || (live.size() < 3'000 && rng.below(100) < 55);
        if (allocate) {
            void* p = pool.allocate();
            ASSERT_TRUE(this->aligned(p));
            this->fill(p, next_tag);
            live.emplace_back(p, next_tag);
            ++next_tag;
        } else {
            const std::size_t i = rng.below(live.size());
            ASSERT_TRUE(this->intact(live[i].first, live[i].second)) << "step " << step;
            pool.deallocate(live[i].first);
            live[i] = live.back();
            live.pop_back();
        }
        ASSERT_EQ(pool.in_use(), live.size()) << "step " << step;
        ASSERT_GE(pool.capacity(), pool.in_use());
    }
    std::set<void*> distinct;
    for (const auto& [p, tag] : live) {
        ASSERT_TRUE(this->intact(p, tag));
        ASSERT_TRUE(distinct.insert(p).second);
        pool.deallocate(p);
    }
    EXPECT_EQ(pool.in_use(), 0U);
}

// --- The allocator adapter, which the pooled book is built on --------------------------

TEST(PoolAllocator, AStdMapTakesItsNodesFromThePool) {
    using Value = std::pair<const int, int>;
    std::map<int, int, std::less<int>, PoolAllocator<Value>> map;
    for (int i = 0; i < 10'000; ++i) {
        map[i * 3] = i;
    }
    ASSERT_EQ(map.size(), 10'000U);
    for (int i = 0; i < 10'000; ++i) {
        const auto it = map.find(i * 3);
        ASSERT_NE(it, map.end());
        ASSERT_EQ(it->second, i);
    }
    for (int i = 0; i < 10'000; i += 2) {
        ASSERT_EQ(map.erase(i * 3), 1U);
    }
    ASSERT_EQ(map.size(), 5'000U);
    int expected = 3;
    for (const auto& [key, value] : map) {
        ASSERT_EQ(key, expected);
        ASSERT_EQ(value, expected / 3);
        expected += 6;
    }
}

TEST(PoolAllocator, AStdUnorderedMapWorksAndItsBucketArrayBypassesThePool) {
    // The bucket array is one allocation of many pointers, which a pool of
    // single slots cannot serve; the adapter sends it to operator new. If it
    // did not, this would crash or corrupt the nodes.
    using Value = std::pair<const std::uint64_t, std::uint64_t>;
    std::unordered_map<std::uint64_t, std::uint64_t, std::hash<std::uint64_t>,
                       std::equal_to<std::uint64_t>, PoolAllocator<Value>>
        map;
    for (std::uint64_t i = 0; i < 50'000; ++i) {
        map.emplace(i * 977, i);
    }
    ASSERT_EQ(map.size(), 50'000U);
    for (std::uint64_t i = 0; i < 50'000; ++i) {
        const auto it = map.find(i * 977);
        ASSERT_NE(it, map.end());
        ASSERT_EQ(it->second, i);
    }
    map.clear();
    EXPECT_TRUE(map.empty());
}

TEST(PoolAllocator, ContainersOfOneNodeTypeShareAPoolAndRecycleThroughIt) {
    using Value = std::pair<const long, long>;
    using Map = std::map<long, long, std::less<long>, PoolAllocator<Value>>;
    {
        Map warm_up;
        for (long i = 0; i < 2'000; ++i) {
            warm_up[i] = i;
        }
    }
    // Everything the first map used has been returned. A second map of the
    // same size should fit in the memory the pool already has.
    Map first;
    for (long i = 0; i < 1'000; ++i) {
        first[i] = i;
    }
    Map second;
    for (long i = 0; i < 1'000; ++i) {
        second[i] = -i;
    }
    for (long i = 0; i < 1'000; ++i) {
        ASSERT_EQ(first.at(i), i);
        ASSERT_EQ(second.at(i), -i);
    }
    EXPECT_TRUE((PoolAllocator<Value>{} == PoolAllocator<Value>{}));
    EXPECT_TRUE((PoolAllocator<Value>{} == PoolAllocator<int>{}));
}

}  // namespace
