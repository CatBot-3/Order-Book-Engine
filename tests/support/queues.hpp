#pragma once

#include <gtest/gtest.h>

#include "obe/util/channels.hpp"
#include "obe/util/mutex_queue.hpp"
#include "obe/util/spsc_ring.hpp"

// The queues and channels the concurrency tests run against.
//
// The same split as the book and the engine. In the passing suite the tests
// run on MutexQueue. Built with OBE_TEST_HAND_WRITTEN defined, the same tests
// run on SpscRing, the lock-free ring written by hand in phase 6, and are
// labelled needs-your-code until it exists.

namespace obe::test {

struct MutexQueueKind {
    template <class T>
    using Queue = util::MutexQueue<T>;
};

struct SpscRingKind {
    template <class T>
    using Queue = util::SpscRing<T>;
};

#if defined(OBE_TEST_HAND_WRITTEN)

using QueueKinds = ::testing::Types<SpscRingKind>;
using ChannelKinds = ::testing::Types<util::RingChannelKind>;

#else

using QueueKinds = ::testing::Types<MutexQueueKind>;
using ChannelKinds = ::testing::Types<util::MutexChannelKind, util::MutexSpinChannelKind>;

#endif

}  // namespace obe::test
