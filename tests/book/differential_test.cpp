#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "obe/book/bbo_hash.hpp"
#include "obe/book/book_manager.hpp"
#include "obe/book/implementations.hpp"
#include "obe/feed/parser.hpp"
#include "obe/gen/synthetic_feed.hpp"
#include "support/implementations.hpp"
#include "support/printers.hpp"

// The differential test: success criterion 3 on synthetic flow.
//
// Each optimized implementation replays the same stream as the reference book.
// For every security the two must publish the same best-bid-and-offer updates,
// in the same order, with the same timestamps, which is what equal BboHasher
// values mean. They must also end with the same counters and the same book.
//
// This is the safety net that makes optimizing safe: a change that speeds the
// book up by getting it wrong fails here. scripts/diff_books.sh runs the same
// comparison on a full real day, and replay_bench repeats it before it will
// report an implementation's timings as valid.

namespace {

using namespace obe;

template <class Impl>
using HashedBook = book::BookManager<typename Impl::Store, typename Impl::Levels, book::BboHasher>;

template <class Impl>
std::unique_ptr<HashedBook<Impl>> replay(std::span<const std::byte> stream, std::size_t reserve) {
    auto manager =
        std::make_unique<HashedBook<Impl>>(book::make_store<typename Impl::Store>(reserve));
    feed::ItchParser parser(*manager);
    EXPECT_TRUE(parser.parse(stream).ok());
    return manager;
}

template <class Impl>
class Differential : public ::testing::Test {};
TYPED_TEST_SUITE(Differential, test::BookTypes);

template <class Impl>
void expect_same_as_reference(const gen::SyntheticConfig& cfg, std::size_t reserve) {
    const std::vector<std::byte> stream = gen::make_synthetic_feed(cfg);
    const auto reference = replay<book::ReferenceImpl>(stream, 0);
    const auto candidate = replay<Impl>(stream, reserve);

    ASSERT_EQ(reference->counters(), book::Counters{}) << "the generated stream is inconsistent";
    EXPECT_EQ(candidate->counters(), reference->counters());
    EXPECT_EQ(candidate->stats(), reference->stats());

    // The same update stream for every security.
    EXPECT_EQ(candidate->listener().total_events(), reference->listener().total_events());
    std::uint32_t securities = 0;
    for (std::uint32_t i = 0; i <= cfg.symbols; ++i) {
        const auto locate = static_cast<Locate>(i);
        ASSERT_EQ(candidate->listener().events(locate), reference->listener().events(locate))
            << "locate " << i << " published a different number of updates";
        ASSERT_EQ(candidate->listener().hash(locate), reference->listener().hash(locate))
            << "locate " << i << " published a different update stream";
        securities += reference->listener().events(locate) != 0 ? 1U : 0U;
    }
    EXPECT_EQ(securities, cfg.symbols) << "every generated security should have traded";
    EXPECT_EQ(candidate->listener().combined(), reference->listener().combined());

    // The same book at the end, below the top as well as at it.
    const book::Audit want = reference->audit();
    const book::Audit got = candidate->audit();
    EXPECT_EQ(got.levels, want.levels);
    EXPECT_EQ(got.level_shares, want.level_shares);
    EXPECT_EQ(got.open_shares, want.open_shares);
    EXPECT_EQ(got.open_orders, want.open_orders);
    EXPECT_EQ(got.empty_levels, 0U);
    for (std::uint32_t i = 1; i <= cfg.symbols; ++i) {
        const auto locate = static_cast<Locate>(i);
        EXPECT_EQ(candidate->book(locate).bbo(), reference->book(locate).bbo()) << "locate " << i;
        EXPECT_EQ(candidate->book(locate).bids().size(), reference->book(locate).bids().size());
        EXPECT_EQ(candidate->book(locate).asks().size(), reference->book(locate).asks().size());
        EXPECT_EQ(candidate->book(locate).executed_shares(),
                  reference->book(locate).executed_shares());
    }
}

TYPED_TEST(Differential, PublishesTheSameUpdateStreamAsTheReference) {
    for (const std::uint64_t seed : {11ULL, 12ULL, 20261004ULL}) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        expect_same_as_reference<TypeParam>(
            {.seed = seed, .symbols = 40, .messages = 200'000, .target_live_orders = 5'000}, 0);
    }
}

TYPED_TEST(Differential, AgreesWhenTheOrderStoreIsPreSized) {
    // Exactly enough, far too little and far too much room. The reservation
    // must never change what the book publishes.
    for (const std::size_t reserve : {std::size_t{5'000}, std::size_t{8}, std::size_t{1} << 20}) {
        SCOPED_TRACE(::testing::Message() << "reserve " << reserve);
        expect_same_as_reference<TypeParam>(
            {.seed = 31, .symbols = 20, .messages = 100'000, .target_live_orders = 5'000}, reserve);
    }
}

TYPED_TEST(Differential, AgreesOnDeepBooksWithFewSecurities) {
    // Two securities and many resting orders: long sides, so updates land far
    // from the best price as well as near it.
    expect_same_as_reference<TypeParam>(
        {.seed = 7, .symbols = 2, .messages = 150'000, .target_live_orders = 20'000}, 0);
}

TYPED_TEST(Differential, AgreesOnManyThinBooks) {
    // The opposite shape: thousands of securities with a handful of orders
    // each, so sides are created, emptied and recreated constantly.
    expect_same_as_reference<TypeParam>(
        {.seed = 8, .symbols = 3'000, .messages = 150'000, .target_live_orders = 6'000}, 0);
}

TYPED_TEST(Differential, PrefetchHintsNeverChangeTheBook) {
    // A prefetch is a hint about memory. Calling it for every order, live or
    // not, and for reference numbers that never existed, must have no visible
    // effect.
    const std::vector<std::byte> stream = gen::make_synthetic_feed(
        {.seed = 9, .symbols = 10, .messages = 50'000, .target_live_orders = 2'000});
    const auto plain = replay<TypeParam>(stream, 0);

    auto hinted = std::make_unique<HashedBook<TypeParam>>();
    feed::ItchParser parser(*hinted);
    feed::FrameReader reader(stream);
    feed::Frame frame;
    OrderId ref = 0;
    while (!reader.done()) {
        ASSERT_EQ(reader.next(frame), feed::ParseStatus::Ok);
        if (feed::peek_order_ref(frame, ref)) {
            hinted->prefetch(ref);
            hinted->prefetch(ref + 1'000'000'007ULL);
            hinted->prefetch(~ref);
        }
        ASSERT_EQ(parser.dispatch(frame), feed::ParseStatus::Ok);
    }
    EXPECT_EQ(hinted->listener().combined(), plain->listener().combined());
    EXPECT_EQ(hinted->counters(), book::Counters{});
    EXPECT_EQ(hinted->orders().size(), plain->orders().size());
}

}  // namespace
