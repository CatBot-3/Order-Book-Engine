#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "obe/util/queue_concepts.hpp"
#include "support/queues.hpp"
#include "support/threads.hpp"

// The judge for a Channel (obe/util/queue_concepts.hpp): a queue a thread can
// wait on, that can be closed by its producer and cancelled by anybody.
//
// In the passing suite it runs on the mutex queue, sleeping and spinning. In
// the hand-written suite it runs on the lock-free ring behind SpinChannel,
// which is how the pipeline uses the ring.

namespace {

using namespace obe;
using namespace std::chrono_literals;

template <class Kind>
class ChannelTest : public ::testing::Test {
 protected:
    template <class T>
    using Channel = typename Kind::template Channel<T>;
};
TYPED_TEST_SUITE(ChannelTest, test::ChannelKinds);

// Cancels a channel when the test leaves the scope, by any route. Declared
// after a Worker, it runs before the Worker's destructor joins the thread, so
// a failed assertion cannot leave that thread waiting on the channel for ever.
template <class C>
struct CancelOnExit {
    C& channel;
    ~CancelOnExit() { channel.cancel(); }
};
template <class C>
CancelOnExit(C&) -> CancelOnExit<C>;

// Long enough for the other thread to have reached its wait, short enough not
// to slow the suite. Nothing depends on it for correctness: a test that
// sleeps too little still passes, it just exercises the non-waiting path.
constexpr auto kSettle = 30ms;

TYPED_TEST(ChannelTest, DeliversInOrderOnOneThread) {
    typename TestFixture::template Channel<int> channel(8);
    EXPECT_GE(channel.capacity(), 8U);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(channel.push(i));
    }
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        ASSERT_TRUE(channel.pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(channel.cancelled());
}

TYPED_TEST(ChannelTest, NothingWaitedMeansNoWaitsCounted) {
    typename TestFixture::template Channel<int> channel(8);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(channel.push(i));
    }
    int out = 0;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(channel.pop(out));
    }
    EXPECT_EQ(channel.stats().push_waits, 0U);
    EXPECT_EQ(channel.stats().pop_waits, 0U);
}

TYPED_TEST(ChannelTest, AfterCloseTheRemainingItemsComeOutAndThenPopSaysNoMore) {
    typename TestFixture::template Channel<std::string> channel(8);
    ASSERT_TRUE(channel.push(std::string("one")));
    ASSERT_TRUE(channel.push(std::string("two")));
    channel.close();
    std::string out;
    ASSERT_TRUE(channel.pop(out));
    EXPECT_EQ(out, "one");
    ASSERT_TRUE(channel.pop(out));
    EXPECT_EQ(out, "two");
    EXPECT_FALSE(channel.pop(out));
    EXPECT_FALSE(channel.pop(out)) << "and it keeps saying so";
    EXPECT_FALSE(channel.cancelled()) << "closing is not cancelling";
}

TYPED_TEST(ChannelTest, PopOnAClosedEmptyChannelReturnsAtOnce) {
    typename TestFixture::template Channel<int> channel(4);
    channel.close();
    int out = -1;
    EXPECT_FALSE(channel.pop(out));
    EXPECT_EQ(out, -1);
}

TYPED_TEST(ChannelTest, PopWaitsUntilAnItemArrives) {
    typename TestFixture::template Channel<int> channel(4);
    test::Abort abort;
    std::atomic<int> received{-1};
    test::Worker consumer(abort, [&] {
        int out = -1;
        if (channel.pop(out)) {
            received.store(out);
        }
    });
    const CancelOnExit guard{channel};
    std::this_thread::sleep_for(kSettle);
    EXPECT_EQ(received.load(), -1) << "pop returned before anything was pushed";
    ASSERT_TRUE(channel.push(42));
    consumer.join();
    EXPECT_EQ(received.load(), 42);
    EXPECT_LE(channel.stats().pop_waits, 1U);
}

TYPED_TEST(ChannelTest, PushWaitsUntilThereIsRoom) {
    typename TestFixture::template Channel<std::size_t> channel(2);
    const std::size_t capacity = channel.capacity();
    test::Abort abort;
    std::atomic<bool> done{false};
    test::Worker producer(abort, [&] {
        for (std::size_t i = 0; i <= capacity; ++i) {  // one more than fits
            if (!channel.push(i)) {
                return;
            }
        }
        done.store(true);
    });
    const CancelOnExit guard{channel};
    std::this_thread::sleep_for(kSettle);
    EXPECT_FALSE(done.load()) << "push returned with the queue full";
    std::size_t out = 99;
    ASSERT_TRUE(channel.pop(out));
    EXPECT_EQ(out, 0U);
    producer.join();
    EXPECT_TRUE(done.load());
    for (std::size_t i = 1; i <= capacity; ++i) {
        ASSERT_TRUE(channel.pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_LE(channel.stats().push_waits, 1U);
}

TYPED_TEST(ChannelTest, CloseReleasesAWaitingPop) {
    typename TestFixture::template Channel<int> channel(4);
    test::Abort abort;
    std::atomic<int> result{-1};
    test::Worker consumer(abort, [&] {
        int out = 0;
        result.store(channel.pop(out) ? 1 : 0);
    });
    const CancelOnExit guard{channel};
    std::this_thread::sleep_for(kSettle);
    channel.close();
    consumer.join();
    EXPECT_EQ(result.load(), 0);
}

TYPED_TEST(ChannelTest, CancelReleasesAWaitingPop) {
    typename TestFixture::template Channel<int> channel(4);
    test::Abort abort;
    std::atomic<int> result{-1};
    test::Worker consumer(abort, [&] {
        int out = 0;
        result.store(channel.pop(out) ? 1 : 0);
    });
    const CancelOnExit guard{channel};
    std::this_thread::sleep_for(kSettle);
    channel.cancel();
    consumer.join();
    EXPECT_EQ(result.load(), 0);
    EXPECT_TRUE(channel.cancelled());
}

TYPED_TEST(ChannelTest, CancelReleasesAWaitingPush) {
    typename TestFixture::template Channel<std::size_t> channel(2);
    const std::size_t capacity = channel.capacity();
    for (std::size_t i = 0; i < capacity; ++i) {
        ASSERT_TRUE(channel.push(i));
    }
    test::Abort abort;
    std::atomic<int> result{-1};
    test::Worker producer(abort, [&] { result.store(channel.push(std::size_t{99}) ? 1 : 0); });
    const CancelOnExit guard{channel};
    std::this_thread::sleep_for(kSettle);
    EXPECT_EQ(result.load(), -1) << "push returned with the queue full";
    channel.cancel();
    producer.join();
    EXPECT_EQ(result.load(), 0);
}

TYPED_TEST(ChannelTest, AfterCancelNothingGoesInOrComesOut) {
    typename TestFixture::template Channel<int> channel(4);
    ASSERT_TRUE(channel.push(1));
    channel.cancel();
    EXPECT_TRUE(channel.cancelled());
    EXPECT_FALSE(channel.push(2));
    int out = -1;
    EXPECT_FALSE(channel.pop(out)) << "cancel abandons what was queued";
    EXPECT_EQ(out, -1);
}

TYPED_TEST(ChannelTest, AStreamOfItemsThenCloseArrivesCompleteAndInOrder) {
    for (const std::size_t capacity : {1U, 2U, 64U, 4096U}) {
        SCOPED_TRACE("capacity " + std::to_string(capacity));
        typename TestFixture::template Channel<std::uint64_t> channel(capacity);
        constexpr std::uint64_t kItems = 50'000;
        test::Abort abort;
        test::Worker producer(abort, [&] {
            for (std::uint64_t i = 0; i < kItems; ++i) {
                if (!channel.push(i)) {
                    return;
                }
            }
            channel.close();
        });
        const CancelOnExit guard{channel};
        std::uint64_t expected = 0;
        std::uint64_t out = 0;
        while (channel.pop(out)) {
            ASSERT_EQ(out, expected);
            ++expected;
        }
        producer.join();
        EXPECT_EQ(expected, kItems);
    }
}

TYPED_TEST(ChannelTest, TheLastItemBeforeACloseIsNeverLost) {
    // The producer pushes one item and closes straight away, while the
    // consumer is already waiting. If pop decided "closed and empty" from a
    // look at the queue made before the close, the item would vanish. Repeated
    // many times, because the window is narrow.
    for (int round = 0; round < 2'000; ++round) {
        typename TestFixture::template Channel<int> channel(4);
        test::Abort abort;
        test::Worker producer(abort, [&] {
            if (channel.push(round)) {
                channel.close();
            }
        });
        const CancelOnExit guard{channel};
        int out = -1;
        const bool got = channel.pop(out);
        int extra = -1;
        const bool more = got && channel.pop(extra);
        channel.cancel();
        producer.join();
        ASSERT_TRUE(got) << "round " << round;
        ASSERT_EQ(out, round);
        ASSERT_FALSE(more);
    }
}

TYPED_TEST(ChannelTest, CarriesValuesThatCanOnlyBeMoved) {
    typename TestFixture::template Channel<std::unique_ptr<int>> channel(1);
    test::Abort abort;
    test::Worker producer(abort, [&] {
        for (int i = 0; i < 1'000; ++i) {
            if (!channel.push(std::make_unique<int>(i))) {
                return;
            }
        }
        channel.close();
    });
    const CancelOnExit guard{channel};
    int expected = 0;
    std::unique_ptr<int> out;
    while (channel.pop(out)) {
        ASSERT_NE(out, nullptr);
        ASSERT_EQ(*out, expected);
        ++expected;
    }
    channel.cancel();
    producer.join();
    EXPECT_EQ(expected, 1'000);
}

}  // namespace
