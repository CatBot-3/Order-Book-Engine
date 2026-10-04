#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "obe/gen/rng.hpp"
#include "obe/util/backoff.hpp"
#include "obe/util/queue_concepts.hpp"
#include "support/queues.hpp"
#include "support/threads.hpp"

// The judge for anything that claims to be a bounded single-producer
// single-consumer queue. These tests assume only the BoundedQueue contract in
// obe/util/queue_concepts.hpp.
//
// The first half uses one thread and pins down the behaviour: order, full,
// empty, what happens to a value that could not be pushed. The second half
// uses two threads. Run under ThreadSanitizer (the `tsan` preset), the second
// half is also the check that the queue's memory ordering is right: on x86 a
// ring with every ordering wrong still passes a plain run, because the
// hardware is stricter than the language.

namespace {

using namespace obe;

template <class Kind>
class BoundedQueueTest : public ::testing::Test {
 protected:
    template <class T>
    using Queue = typename Kind::template Queue<T>;
};
TYPED_TEST_SUITE(BoundedQueueTest, test::QueueKinds);

// --- One thread --------------------------------------------------------------

TYPED_TEST(BoundedQueueTest, StartsEmpty) {
    typename TestFixture::template Queue<int> queue(8);
    int out = -1;
    EXPECT_FALSE(queue.try_pop(out));
    EXPECT_EQ(out, -1) << "a failed pop must leave its argument alone";
}

TYPED_TEST(BoundedQueueTest, HoldsAtLeastWhatWasAskedFor) {
    for (const std::size_t asked : {1U, 2U, 3U, 4U, 5U, 7U, 8U, 9U, 100U, 1000U, 1024U, 1025U}) {
        typename TestFixture::template Queue<std::size_t> queue(asked);
        ASSERT_GE(queue.capacity(), asked);
        // And it really does hold capacity() items, and not one more.
        for (std::size_t i = 0; i < queue.capacity(); ++i) {
            ASSERT_TRUE(queue.try_push(i)) << "item " << i << " of " << queue.capacity();
        }
        ASSERT_FALSE(queue.try_push(asked)) << "capacity " << queue.capacity();
        for (std::size_t i = 0; i < queue.capacity(); ++i) {
            std::size_t out = 0;
            ASSERT_TRUE(queue.try_pop(out));
            ASSERT_EQ(out, i);
        }
        std::size_t out = 0;
        ASSERT_FALSE(queue.try_pop(out));
    }
}

TYPED_TEST(BoundedQueueTest, ItemsComeOutInTheOrderTheyWentIn) {
    typename TestFixture::template Queue<int> queue(8);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(queue.try_push(i * 10));
    }
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, i * 10);
    }
}

TYPED_TEST(BoundedQueueTest, AFullQueueRefusesAndLeavesTheValueIntact) {
    typename TestFixture::template Queue<std::string> queue(2);
    const std::size_t capacity = queue.capacity();
    for (std::size_t i = 0; i < capacity; ++i) {
        ASSERT_TRUE(queue.try_push(std::string("item ") + std::to_string(i)));
    }
    // Long enough not to fit a small-string buffer: a moved-from copy would
    // be visibly empty.
    const std::string original(100, 'x');
    std::string value = original;
    EXPECT_FALSE(queue.try_push(std::move(value)));
    // NOLINTNEXTLINE(bugprone-use-after-move): the contract says it was not moved.
    EXPECT_EQ(value, original) << "the caller will retry with this object";

    // The refused push changed nothing inside either.
    for (std::size_t i = 0; i < capacity; ++i) {
        std::string out;
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, "item " + std::to_string(i));
    }
    // Now there is room, and the same object can be pushed.
    EXPECT_TRUE(queue.try_push(std::move(value)));
    std::string out;
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, original);
}

TYPED_TEST(BoundedQueueTest, CanBeFilledAndDrainedAgainAndAgain) {
    typename TestFixture::template Queue<std::uint64_t> queue(4);
    const std::size_t capacity = queue.capacity();
    std::uint64_t next_in = 0;
    std::uint64_t next_out = 0;
    // Many more items than slots, so the indices go round the buffer many
    // times, and with a different fill level each lap.
    for (int lap = 0; lap < 5'000; ++lap) {
        const std::size_t burst = 1 + static_cast<std::size_t>(lap) % capacity;
        for (std::size_t i = 0; i < burst; ++i) {
            ASSERT_TRUE(queue.try_push(next_in++));
        }
        for (std::size_t i = 0; i < burst; ++i) {
            std::uint64_t out = 0;
            ASSERT_TRUE(queue.try_pop(out));
            ASSERT_EQ(out, next_out++);
        }
        std::uint64_t out = 0;
        ASSERT_FALSE(queue.try_pop(out));
    }
}

TYPED_TEST(BoundedQueueTest, BehavesLikeABoundedDequeOnRandomOperations) {
    for (const std::size_t asked : {1U, 3U, 16U, 100U}) {
        typename TestFixture::template Queue<std::uint64_t> queue(asked);
        std::deque<std::uint64_t> model;
        gen::SplitMix64 rng(asked);
        std::uint64_t next = 0;
        for (int step = 0; step < 50'000; ++step) {
            if (rng.below(100) < 52) {
                const bool pushed = queue.try_push(next);
                ASSERT_EQ(pushed, model.size() < queue.capacity()) << "step " << step;
                if (pushed) {
                    model.push_back(next);
                }
                ++next;
            } else {
                std::uint64_t out = ~std::uint64_t{0};
                const bool popped = queue.try_pop(out);
                ASSERT_EQ(popped, !model.empty()) << "step " << step;
                if (popped) {
                    ASSERT_EQ(out, model.front()) << "step " << step;
                    model.pop_front();
                }
            }
        }
    }
}

TYPED_TEST(BoundedQueueTest, WorksWithAValueThatCanOnlyBeMoved) {
    typename TestFixture::template Queue<std::unique_ptr<int>> queue(4);
    ASSERT_TRUE(queue.try_push(std::make_unique<int>(7)));
    ASSERT_TRUE(queue.try_push(std::make_unique<int>(8)));
    std::unique_ptr<int> out;
    ASSERT_TRUE(queue.try_pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 7);
    ASSERT_TRUE(queue.try_pop(out));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, 8);
}

TYPED_TEST(BoundedQueueTest, APoppedItemIsMovedOutNotCopied) {
    const auto item = std::make_shared<int>(1);
    typename TestFixture::template Queue<std::shared_ptr<int>> queue(4);
    ASSERT_TRUE(queue.try_push(item));
    EXPECT_EQ(item.use_count(), 2);
    {
        std::shared_ptr<int> out;
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, item);
        // Us and `out`. A third owner would be a copy left behind in the slot.
        EXPECT_EQ(item.use_count(), 2);
    }
    EXPECT_EQ(item.use_count(), 1);
}

TYPED_TEST(BoundedQueueTest, DestroyingTheQueueReleasesWhatIsStillInIt) {
    const auto item = std::make_shared<int>(1);
    {
        typename TestFixture::template Queue<std::shared_ptr<int>> queue(8);
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(queue.try_push(item));
        }
        EXPECT_EQ(item.use_count(), 4);
    }
    EXPECT_EQ(item.use_count(), 1);
}

// --- Two threads -------------------------------------------------------------

// A record whose fields are tied together. If a consumer ever read a slot
// before its producer had finished writing it, the ties would not hold.
struct Linked {
    std::uint64_t index = 0;
    std::uint64_t triple = 0;
    std::uint64_t inverse = 0;
    std::uint64_t padding[4] = {};

    static Linked make(std::uint64_t i) { return {i, i * 3, ~i, {i, i, i, i}}; }
    [[nodiscard]] bool consistent() const {
        return triple == index * 3 && inverse == ~index && padding[0] == index &&
               padding[3] == index;
    }
};

// One thread pushes 0, 1, 2, ... and the test's own thread pops and checks.
// `producer_pause` and `consumer_pause` make one side slow now and then, so
// that the queue spends time full, or spends time empty.
template <class Queue>
void transfer_in_order(Queue& queue, std::uint64_t items, std::uint64_t producer_pause,
                       std::uint64_t consumer_pause) {
    test::Abort abort;
    test::Worker producer(abort, [&] {
        for (std::uint64_t i = 0; i < items && !abort.raised(); ++i) {
            util::Backoff backoff;
            while (!queue.try_push(Linked::make(i))) {
                if (abort.raised()) {
                    return;
                }
                backoff.pause();
            }
            if (producer_pause != 0 && i % producer_pause == 0) {
                std::this_thread::yield();
            }
        }
    });
    const test::StopOnExit stop(abort);

    for (std::uint64_t expected = 0; expected < items; ++expected) {
        Linked out;
        util::Backoff backoff;
        while (!queue.try_pop(out)) {
            ASSERT_FALSE(abort.raised()) << "the producer failed";
            backoff.pause();
        }
        ASSERT_EQ(out.index, expected) << "an item was lost, repeated or reordered";
        ASSERT_TRUE(out.consistent()) << "item " << expected << " was read half-written";
        if (consumer_pause != 0 && expected % consumer_pause == 0) {
            std::this_thread::yield();
        }
    }
    producer.join();
    Linked out;
    EXPECT_FALSE(queue.try_pop(out)) << "more came out than went in";
}

constexpr std::uint64_t kItems = 400'000;

TYPED_TEST(BoundedQueueTest, TwoThreadsTransferEveryItemInOrder) {
    for (const std::size_t capacity : {1U, 2U, 64U, 4096U}) {
        SCOPED_TRACE("capacity " + std::to_string(capacity));
        typename TestFixture::template Queue<Linked> queue(capacity);
        transfer_in_order(queue, kItems, 0, 0);
        ASSERT_FALSE(this->HasFatalFailure());
    }
}

TYPED_TEST(BoundedQueueTest, ASlowConsumerKeepsTheQueueFull) {
    typename TestFixture::template Queue<Linked> queue(8);
    transfer_in_order(queue, kItems / 4, 0, 7);
}

TYPED_TEST(BoundedQueueTest, ASlowProducerKeepsTheQueueEmpty) {
    typename TestFixture::template Queue<Linked> queue(8);
    transfer_in_order(queue, kItems / 4, 7, 0);
}

TYPED_TEST(BoundedQueueTest, TwoThreadsTransferValuesThatOwnMemory) {
    // Each item is a heap allocation made on one thread and freed on the
    // other. A slot read too early would hand over a pointer to nothing, which
    // AddressSanitizer reports.
    typename TestFixture::template Queue<std::unique_ptr<std::uint64_t>> queue(32);
    constexpr std::uint64_t kOwned = 100'000;
    test::Abort abort;
    test::Worker producer(abort, [&] {
        for (std::uint64_t i = 0; i < kOwned && !abort.raised(); ++i) {
            auto item = std::make_unique<std::uint64_t>(i);
            util::Backoff backoff;
            while (!queue.try_push(std::move(item))) {
                if (abort.raised()) {
                    return;
                }
                backoff.pause();
            }
        }
    });
    const test::StopOnExit stop(abort);
    for (std::uint64_t expected = 0; expected < kOwned; ++expected) {
        std::unique_ptr<std::uint64_t> out;
        util::Backoff backoff;
        while (!queue.try_pop(out)) {
            ASSERT_FALSE(abort.raised()) << "the producer failed";
            backoff.pause();
        }
        ASSERT_NE(out, nullptr);
        ASSERT_EQ(*out, expected);
    }
    producer.join();
}

}  // namespace
