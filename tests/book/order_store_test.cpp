#include <gtest/gtest.h>

#include <concepts>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "obe/book/concepts.hpp"
#include "obe/gen/synthetic_feed.hpp"
#include "support/implementations.hpp"

// The judge for anything that claims to be an order store. These tests assume
// only the OrderStoreLike contract in obe/book/concepts.hpp.

namespace {

using namespace obe;
using book::OrderRecord;

template <class Store>
class OrderStoreTest : public ::testing::Test {
 protected:
    Store store;
};
TYPED_TEST_SUITE(OrderStoreTest, test::OrderStoreTypes);

constexpr OrderRecord kBuy{.price = 1'000'000, .qty = 300, .side = Side::Buy};
constexpr OrderRecord kSell{.price = 1'010'000, .qty = 500, .side = Side::Sell};

TYPED_TEST(OrderStoreTest, StartsEmpty) {
    EXPECT_EQ(this->store.size(), 0U);
    EXPECT_EQ(this->store.find(1), nullptr);
    EXPECT_FALSE(this->store.erase(1));
}

TYPED_TEST(OrderStoreTest, InsertThenFindReturnsTheRecord) {
    ASSERT_TRUE(this->store.insert(42, kBuy));
    EXPECT_EQ(this->store.size(), 1U);
    const OrderRecord* found = this->store.find(42);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, kBuy);
    EXPECT_EQ(this->store.find(43), nullptr);
}

TYPED_TEST(OrderStoreTest, InsertingAnExistingIdIsRefusedAndKeepsTheOriginal) {
    ASSERT_TRUE(this->store.insert(42, kBuy));
    EXPECT_FALSE(this->store.insert(42, kSell));
    EXPECT_EQ(this->store.size(), 1U);
    ASSERT_NE(this->store.find(42), nullptr);
    EXPECT_EQ(*this->store.find(42), kBuy) << "a refused insert must not overwrite";
}

TYPED_TEST(OrderStoreTest, FindGivesAPointerTheBookCanWriteThrough) {
    // BookManager reduces an order's quantity in place on every execution.
    ASSERT_TRUE(this->store.insert(7, kBuy));
    OrderRecord* found = this->store.find(7);
    ASSERT_NE(found, nullptr);
    found->qty -= 100;
    ASSERT_NE(this->store.find(7), nullptr);
    EXPECT_EQ(this->store.find(7)->qty, 200U);
}

TYPED_TEST(OrderStoreTest, ConstFindSeesTheSameRecord) {
    ASSERT_TRUE(this->store.insert(7, kSell));
    const TypeParam& const_store = this->store;
    const OrderRecord* found = const_store.find(7);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(*found, kSell);
    EXPECT_EQ(const_store.find(8), nullptr);
}

TYPED_TEST(OrderStoreTest, EraseRemovesExactlyThatOrder) {
    ASSERT_TRUE(this->store.insert(1, kBuy));
    ASSERT_TRUE(this->store.insert(2, kSell));
    EXPECT_TRUE(this->store.erase(1));
    EXPECT_EQ(this->store.size(), 1U);
    EXPECT_EQ(this->store.find(1), nullptr);
    ASSERT_NE(this->store.find(2), nullptr);
    EXPECT_EQ(*this->store.find(2), kSell);
    EXPECT_FALSE(this->store.erase(1)) << "erasing twice must report 'not present'";
    EXPECT_EQ(this->store.size(), 1U);
}

TYPED_TEST(OrderStoreTest, AnErasedIdCanBeInsertedAgain) {
    ASSERT_TRUE(this->store.insert(5, kBuy));
    ASSERT_TRUE(this->store.erase(5));
    EXPECT_TRUE(this->store.insert(5, kSell));
    ASSERT_NE(this->store.find(5), nullptr);
    EXPECT_EQ(*this->store.find(5), kSell);
}

TYPED_TEST(OrderStoreTest, IdsNeedNotBeSmallOrDense) {
    // Reference numbers are 64-bit and nothing promises they are consecutive.
    // (Whether they are dense enough to index an array directly is phase 4
    // experiment 2, and the answer has to come from measuring the real feed.)
    const std::vector<OrderId> ids{0, 1, std::uint64_t{1} << 32, (std::uint64_t{1} << 63) + 12345,
                                   std::numeric_limits<OrderId>::max()};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        ASSERT_TRUE(this->store.insert(ids[i], OrderRecord{.price = static_cast<Price>(i + 1),
                                                           .qty = static_cast<Qty>(10 * (i + 1)),
                                                           .side = Side::Buy}))
            << ids[i];
    }
    EXPECT_EQ(this->store.size(), ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const OrderRecord* found = this->store.find(ids[i]);
        ASSERT_NE(found, nullptr) << ids[i];
        EXPECT_EQ(found->price, static_cast<Price>(i + 1));
    }
}

TYPED_TEST(OrderStoreTest, HoldsManyOrdersThroughGrowthAndShrinkage) {
    constexpr OrderId kCount = 200'000;
    for (OrderId id = 0; id < kCount; ++id) {
        ASSERT_TRUE(this->store.insert(
            id * 3, OrderRecord{.price = static_cast<Price>(id), .qty = 1, .side = Side::Sell}));
    }
    ASSERT_EQ(this->store.size(), kCount);
    for (OrderId id = 0; id < kCount; id += 2) {
        ASSERT_TRUE(this->store.erase(id * 3));
    }
    ASSERT_EQ(this->store.size(), kCount / 2);
    for (OrderId id = 0; id < kCount; ++id) {
        const OrderRecord* found = this->store.find(id * 3);
        if (id % 2 == 0) {
            ASSERT_EQ(found, nullptr) << id;
        } else {
            ASSERT_NE(found, nullptr) << id;
            ASSERT_EQ(found->price, static_cast<Price>(id));
        }
        ASSERT_EQ(this->store.find(id * 3 + 1), nullptr);
    }
}

TYPED_TEST(OrderStoreTest, KeysThatDifferOnlyInTheirHighBitsStayDistinct) {
    // Every one of these keys has the same low 40 bits. A table that picks a
    // slot from the low bits alone sends them all to one place, so this checks
    // that collisions are resolved correctly. It does not time anything, but
    // if it is noticeably slow for your table, the hash function is why.
    constexpr std::uint64_t kCount = 4'000;
    const auto key = [](std::uint64_t i) { return i << 40; };
    for (std::uint64_t i = 0; i < kCount; ++i) {
        ASSERT_TRUE(this->store.insert(
            key(i), OrderRecord{.price = static_cast<Price>(i), .qty = 7, .side = Side::Buy}))
            << i;
    }
    ASSERT_EQ(this->store.size(), kCount);
    for (std::uint64_t i = 0; i < kCount; ++i) {
        const OrderRecord* found = this->store.find(key(i));
        ASSERT_NE(found, nullptr) << i;
        ASSERT_EQ(found->price, static_cast<Price>(i));
        ASSERT_EQ(this->store.find(key(i) + 1), nullptr) << "a neighbouring key must not match";
    }
    for (std::uint64_t i = 0; i < kCount; i += 2) {
        ASSERT_TRUE(this->store.erase(key(i)));
    }
    for (std::uint64_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(this->store.find(key(i)) != nullptr, i % 2 == 1) << i;
    }
    ASSERT_EQ(this->store.size(), kCount / 2);
}

TYPED_TEST(OrderStoreTest, SurvivesLongChurnAtAConstantSize) {
    // The shape of a real day: as many erases as inserts, with the number of
    // live orders roughly level. A window of 2'000 live orders slides over two
    // million reference numbers. An open-addressing table that marks erased
    // slots and never cleans them up fills with markers here: lookups of
    // absent keys get slower and slower, and eventually an insert finds no
    // free slot. If this test crawls or hangs, that is what happened.
    constexpr OrderId kLive = 2'000;
    constexpr OrderId kTotal = 2'000'000;
    for (OrderId id = 0; id < kTotal; ++id) {
        ASSERT_TRUE(this->store.insert(
            id, OrderRecord{.price = static_cast<Price>(id & 0xffff), .qty = 1, .side = Side::Buy}))
            << id;
        if (id >= kLive) {
            ASSERT_TRUE(this->store.erase(id - kLive)) << id;
        }
        if ((id & 0xfff) == 0) {
            const OrderRecord* newest = this->store.find(id);
            ASSERT_NE(newest, nullptr) << id;
            ASSERT_EQ(newest->price, static_cast<Price>(id & 0xffff));
            ASSERT_EQ(this->store.find(id + 1), nullptr) << "not inserted yet";
            if (id >= kLive) {
                ASSERT_EQ(this->store.find(id - kLive), nullptr) << "erased";
                ASSERT_EQ(this->store.size(), kLive);
            }
        }
    }
    ASSERT_EQ(this->store.size(), kLive);
    for (OrderId id = kTotal - kLive; id < kTotal; ++id) {
        ASSERT_NE(this->store.find(id), nullptr) << id;
    }
}

TYPED_TEST(OrderStoreTest, APreSizedStoreStillGrowsPastItsReservation) {
    // Pre-sizing is a hint about the expected peak, not a limit. A day busier
    // than expected must still replay correctly.
    if constexpr (std::constructible_from<TypeParam, std::size_t>) {
        TypeParam small(std::size_t{16});
        constexpr OrderId kCount = 20'000;
        for (OrderId id = 0; id < kCount; ++id) {
            ASSERT_TRUE(small.insert(
                id * 7 + 1,
                OrderRecord{.price = static_cast<Price>(id), .qty = 2, .side = Side::Sell}))
                << id;
        }
        ASSERT_EQ(small.size(), kCount);
        for (OrderId id = 0; id < kCount; ++id) {
            const OrderRecord* found = small.find(id * 7 + 1);
            ASSERT_NE(found, nullptr) << id;
            ASSERT_EQ(found->price, static_cast<Price>(id));
        }
    } else {
        GTEST_SKIP() << "this store has no pre-sizing constructor";
    }
}

TYPED_TEST(OrderStoreTest, MovingAStoreMovesItsOrders) {
    // BookManager is handed its store by value.
    constexpr OrderId kCount = 1'000;
    for (OrderId id = 0; id < kCount; ++id) {
        ASSERT_TRUE(this->store.insert(
            id + 100, OrderRecord{.price = static_cast<Price>(id), .qty = 3, .side = Side::Buy}));
    }
    TypeParam moved(std::move(this->store));
    ASSERT_EQ(moved.size(), kCount);
    for (OrderId id = 0; id < kCount; ++id) {
        const OrderRecord* found = moved.find(id + 100);
        ASSERT_NE(found, nullptr) << id;
        ASSERT_EQ(found->price, static_cast<Price>(id));
    }
    ASSERT_TRUE(moved.insert(5, kBuy)) << "a moved-to store is fully usable";
    ASSERT_TRUE(moved.erase(100));
}

// Model-based test: a long seeded sequence of random operations, checked
// against std::map after every step. Small ids make collisions between
// operations frequent.
TYPED_TEST(OrderStoreTest, AgreesWithAModelUnderRandomOperations) {
    std::map<OrderId, OrderRecord> model;
    gen::SplitMix64 rng(0x5eed);
    for (int step = 0; step < 300'000; ++step) {
        const OrderId id = rng.below(4'000);
        switch (rng.below(4)) {
            case 0: {
                const OrderRecord rec{.price = static_cast<Price>(rng.below(1'000'000)),
                                      .qty = static_cast<Qty>(rng.between(1, 10'000)),
                                      .side = rng.below(2) == 0 ? Side::Buy : Side::Sell};
                const bool inserted = model.emplace(id, rec).second;
                ASSERT_EQ(this->store.insert(id, rec), inserted) << "step " << step;
                break;
            }
            case 1:
                ASSERT_EQ(this->store.erase(id), model.erase(id) == 1) << "step " << step;
                break;
            case 2: {
                OrderRecord* found = this->store.find(id);
                const auto it = model.find(id);
                ASSERT_EQ(found != nullptr, it != model.end()) << "step " << step;
                if (found != nullptr) {
                    ASSERT_EQ(*found, it->second) << "step " << step;
                    // Mutate through the pointer, as the book does.
                    found->qty += 1;
                    it->second.qty += 1;
                }
                break;
            }
            default: {
                const TypeParam& const_store = this->store;
                const OrderRecord* found = const_store.find(id);
                const auto it = model.find(id);
                ASSERT_EQ(found != nullptr, it != model.end()) << "step " << step;
                if (found != nullptr) {
                    ASSERT_EQ(*found, it->second) << "step " << step;
                }
                break;
            }
        }
        ASSERT_EQ(this->store.size(), model.size()) << "step " << step;
    }
}

}  // namespace
