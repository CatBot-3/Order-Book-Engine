#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "obe/book/concepts.hpp"
#include "obe/gen/synthetic_feed.hpp"
#include "support/implementations.hpp"

// The judge for anything that claims to be one side of a book. These tests
// assume only the PriceLevelsLike contract in obe/book/concepts.hpp.

namespace {

using namespace obe;
using book::Level;

template <class Levels>
std::vector<Level> all(const Levels& levels) {
    std::vector<Level> out;
    levels.for_each([&out](const Level& level) {
        out.push_back(level);
        return true;
    });
    return out;
}

template <class Levels>
class PriceLevelsTest : public ::testing::Test {
 protected:
    Levels bids{Side::Buy};
    Levels asks{Side::Sell};
};
TYPED_TEST_SUITE(PriceLevelsTest, test::PriceLevelsTypes);

TYPED_TEST(PriceLevelsTest, StartsEmpty) {
    for (const TypeParam* side : {&this->bids, &this->asks}) {
        EXPECT_TRUE(side->empty());
        EXPECT_EQ(side->size(), 0U);
        EXPECT_EQ(side->best(), std::nullopt);
        EXPECT_TRUE(all(*side).empty());
    }
}

TYPED_TEST(PriceLevelsTest, BestBidIsTheHighestPrice) {
    this->bids.add(1'000'000, 100);
    this->bids.add(1'000'200, 200);
    this->bids.add(1'000'100, 300);
    EXPECT_EQ(this->bids.best(), (Level{1'000'200, 200}));
    EXPECT_EQ(this->bids.size(), 3U);
    EXPECT_FALSE(this->bids.empty());
}

TYPED_TEST(PriceLevelsTest, BestAskIsTheLowestPrice) {
    this->asks.add(1'000'200, 200);
    this->asks.add(1'000'000, 100);
    this->asks.add(1'000'100, 300);
    EXPECT_EQ(this->asks.best(), (Level{1'000'000, 100}));
    EXPECT_EQ(this->asks.size(), 3U);
}

TYPED_TEST(PriceLevelsTest, SharesAtTheSamePriceAddUp) {
    this->asks.add(500'000, 100);
    this->asks.add(500'000, 250);
    EXPECT_EQ(this->asks.size(), 1U) << "the same price is one level, not two";
    EXPECT_EQ(this->asks.best(), (Level{500'000, 350}));
}

TYPED_TEST(PriceLevelsTest, PartialRemoveKeepsTheLevel) {
    this->bids.add(500'000, 300);
    EXPECT_TRUE(this->bids.remove(500'000, 100));
    EXPECT_EQ(this->bids.best(), (Level{500'000, 200}));
    EXPECT_EQ(this->bids.size(), 1U);
}

TYPED_TEST(PriceLevelsTest, RemovingEverythingErasesTheLevelAndBestMovesOn) {
    this->bids.add(500'000, 300);
    this->bids.add(499'900, 700);
    EXPECT_TRUE(this->bids.remove(500'000, 300));
    EXPECT_EQ(this->bids.size(), 1U) << "a level with no shares must not be left behind";
    EXPECT_EQ(this->bids.best(), (Level{499'900, 700}));

    this->asks.add(500'100, 50);
    this->asks.add(500'200, 60);
    EXPECT_TRUE(this->asks.remove(500'100, 50));
    EXPECT_EQ(this->asks.best(), (Level{500'200, 60}));

    EXPECT_TRUE(this->asks.remove(500'200, 60));
    EXPECT_TRUE(this->asks.empty());
    EXPECT_EQ(this->asks.best(), std::nullopt);
}

TYPED_TEST(PriceLevelsTest, RemoveFromAMissingLevelIsRefusedAndChangesNothing) {
    this->asks.add(500'000, 100);
    EXPECT_FALSE(this->asks.remove(500'100, 10));
    EXPECT_EQ(all(this->asks), (std::vector<Level>{{500'000, 100}}));

    EXPECT_FALSE(this->bids.remove(500'000, 10)) << "removing from an empty side";
    EXPECT_TRUE(this->bids.empty());
}

TYPED_TEST(PriceLevelsTest, RemovingMoreThanTheLevelHoldsIsRefusedAndChangesNothing) {
    this->asks.add(500'000, 100);
    EXPECT_FALSE(this->asks.remove(500'000, 101));
    EXPECT_EQ(all(this->asks), (std::vector<Level>{{500'000, 100}}))
        << "a refused remove must not clamp or erase";
}

TYPED_TEST(PriceLevelsTest, ForEachWalksBidsFromHighestToLowest) {
    for (const Price p : {300U, 100U, 500U, 200U, 400U}) {
        this->bids.add(p, p * 2);
    }
    EXPECT_EQ(all(this->bids),
              (std::vector<Level>{{500, 1000}, {400, 800}, {300, 600}, {200, 400}, {100, 200}}));
}

TYPED_TEST(PriceLevelsTest, ForEachWalksAsksFromLowestToHighest) {
    for (const Price p : {300U, 100U, 500U, 200U, 400U}) {
        this->asks.add(p, p * 2);
    }
    EXPECT_EQ(all(this->asks),
              (std::vector<Level>{{100, 200}, {200, 400}, {300, 600}, {400, 800}, {500, 1000}}));
}

TYPED_TEST(PriceLevelsTest, ForEachStopsWhenTheVisitorReturnsFalse) {
    for (Price p = 1; p <= 100; ++p) {
        this->asks.add(p, 10);
    }
    std::vector<Level> seen;
    this->asks.for_each([&seen](const Level& level) {
        seen.push_back(level);
        return seen.size() < 3;
    });
    EXPECT_EQ(seen, (std::vector<Level>{{1, 10}, {2, 10}, {3, 10}}));
}

TYPED_TEST(PriceLevelsTest, ALevelCanHoldMoreSharesThanFitIn32Bits) {
    // Each order is at most 2^32 - 1 shares; a level sums many orders.
    constexpr Qty kBig = 4'000'000'000U;
    this->bids.add(100, kBig);
    this->bids.add(100, kBig);
    this->bids.add(100, kBig);
    EXPECT_EQ(this->bids.best(), (Level{100, 12'000'000'000ULL}));
    EXPECT_TRUE(this->bids.remove(100, kBig));
    EXPECT_EQ(this->bids.best(), (Level{100, 8'000'000'000ULL}));
}

TYPED_TEST(PriceLevelsTest, HandlesThePriceExtremes) {
    constexpr Price kMax = 4'294'967'295U;
    this->bids.add(1, 10);
    this->bids.add(kMax, 20);
    EXPECT_EQ(this->bids.best(), (Level{kMax, 20}));
    this->asks.add(kMax, 20);
    this->asks.add(1, 10);
    EXPECT_EQ(this->asks.best(), (Level{1, 10}));
    this->asks.add(0, 5);  // a zero price is not special to the container
    EXPECT_EQ(this->asks.best(), (Level{0, 5}));
}

TYPED_TEST(PriceLevelsTest, TheTwoSidesAreIndependent) {
    this->bids.add(100, 10);
    this->asks.add(100, 20);
    EXPECT_EQ(this->bids.best(), (Level{100, 10}));
    EXPECT_EQ(this->asks.best(), (Level{100, 20}));
    EXPECT_TRUE(this->bids.remove(100, 10));
    EXPECT_TRUE(this->bids.empty());
    EXPECT_EQ(this->asks.best(), (Level{100, 20}));
}

TYPED_TEST(PriceLevelsTest, ManyLevelsAddedInRandomOrderComeBackSorted) {
    // 5'000 distinct prices in a shuffled order, so most inserts land in the
    // middle of whatever is already there, not at either end.
    constexpr Price kLevels = 5'000;
    std::vector<Price> prices;
    prices.reserve(kLevels);
    for (Price i = 0; i < kLevels; ++i) {
        prices.push_back(100'000 + i * 100);
    }
    gen::SplitMix64 rng(0x5eed);
    for (std::size_t i = prices.size() - 1; i > 0; --i) {
        std::swap(prices[i], prices[rng.below(i + 1)]);
    }
    for (const Price p : prices) {
        this->bids.add(p, 10);
        this->asks.add(p, 20);
    }
    ASSERT_EQ(this->bids.size(), kLevels);
    ASSERT_EQ(this->asks.size(), kLevels);

    const std::vector<Level> bid_walk = all(this->bids);
    const std::vector<Level> ask_walk = all(this->asks);
    ASSERT_EQ(bid_walk.size(), kLevels);
    ASSERT_EQ(ask_walk.size(), kLevels);
    for (Price i = 0; i < kLevels; ++i) {
        ASSERT_EQ(ask_walk[i], (Level{100'000 + i * 100, 20})) << "ask level " << i;
        ASSERT_EQ(bid_walk[i], (Level{100'000 + (kLevels - 1 - i) * 100, 10})) << "bid level " << i;
    }

    // Remove them all in a different shuffled order.
    for (std::size_t i = prices.size() - 1; i > 0; --i) {
        std::swap(prices[i], prices[rng.below(i + 1)]);
    }
    for (const Price p : prices) {
        ASSERT_TRUE(this->bids.remove(p, 10)) << p;
        ASSERT_TRUE(this->asks.remove(p, 20)) << p;
    }
    EXPECT_TRUE(this->bids.empty());
    EXPECT_TRUE(this->asks.empty());
}

TYPED_TEST(PriceLevelsTest, UpdatesDeepInTheBookLeaveTheBestUntouched) {
    for (Price i = 0; i < 300; ++i) {
        this->bids.add(50'000 - i * 100, 100);  // best bid 50'000, worst 20'100
        this->asks.add(50'100 + i * 100, 100);  // best ask 50'100, worst 80'000
    }
    // Add beyond the worst level, then in the middle, then remove them again.
    this->bids.add(10'000, 5);
    this->asks.add(90'000, 5);
    this->bids.add(35'050, 6);
    this->asks.add(65'050, 6);
    EXPECT_EQ(this->bids.size(), 302U);
    EXPECT_EQ(this->asks.size(), 302U);
    EXPECT_EQ(this->bids.best(), (Level{50'000, 100}));
    EXPECT_EQ(this->asks.best(), (Level{50'100, 100}));
    EXPECT_EQ(all(this->bids).back(), (Level{10'000, 5}));
    EXPECT_EQ(all(this->asks).back(), (Level{90'000, 5}));

    EXPECT_TRUE(this->bids.remove(35'050, 6));
    EXPECT_TRUE(this->asks.remove(65'050, 6));
    EXPECT_TRUE(this->bids.remove(10'000, 5));
    EXPECT_TRUE(this->asks.remove(90'000, 5));
    EXPECT_EQ(this->bids.size(), 300U);
    EXPECT_EQ(this->asks.size(), 300U);
    EXPECT_EQ(this->bids.best(), (Level{50'000, 100}));
    EXPECT_EQ(this->asks.best(), (Level{50'100, 100}));
}

// Model-based test: random adds and removes on a narrow price band, checked
// against std::map after every step, for both sides.
TYPED_TEST(PriceLevelsTest, AgreesWithAModelUnderRandomOperations) {
    for (const Side side : {Side::Buy, Side::Sell}) {
        TypeParam levels(side);
        std::map<Price, std::uint64_t> model;
        gen::SplitMix64 rng(side == Side::Buy ? 0xb1d : 0xa5c);

        // The model in best-to-worst order: ascending for asks, descending for bids.
        const auto expected = [&model, side]() {
            std::vector<Level> out;
            out.reserve(model.size());
            for (const auto& [price, qty] : model) {
                out.push_back({price, qty});
            }
            if (side == Side::Buy) {
                std::reverse(out.begin(), out.end());
            }
            return out;
        };

        for (int step = 0; step < 100'000; ++step) {
            const auto price = static_cast<Price>(10'000 + rng.below(64) * 100);
            const auto qty = static_cast<Qty>(rng.between(1, 500));
            if (rng.below(100) < 55) {
                levels.add(price, qty);
                model[price] += qty;
            } else {
                const auto it = model.find(price);
                const bool allowed = it != model.end() && it->second >= qty;
                ASSERT_EQ(levels.remove(price, qty), allowed) << "step " << step;
                if (allowed) {
                    it->second -= qty;
                    if (it->second == 0) {
                        model.erase(it);
                    }
                }
            }
            ASSERT_EQ(levels.size(), model.size()) << "step " << step;
            ASSERT_EQ(levels.empty(), model.empty()) << "step " << step;
            const std::vector<Level> want = expected();
            if (want.empty()) {
                ASSERT_EQ(levels.best(), std::nullopt) << "step " << step;
            } else {
                ASSERT_EQ(levels.best(), want.front()) << "step " << step;
            }
            // The full walk is checked less often; it is the slow part.
            if (step % 97 == 0) {
                ASSERT_EQ(all(levels), want) << "step " << step;
            }
        }
        ASSERT_EQ(all(levels), expected());
    }
}

}  // namespace
