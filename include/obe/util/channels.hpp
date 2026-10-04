#pragma once

#include <string_view>
#include <type_traits>

#include "obe/util/mutex_queue.hpp"
#include "obe/util/queue_concepts.hpp"
#include "obe/util/spin_channel.hpp"
#include "obe/util/spsc_ring.hpp"

// The named channels.
//
// queue_bench, pipeline_bench and the tests select one by name (--queue NAME).
// Each differs from the next in one respect, so that a difference in a
// measurement has one explanation:
//
//   mutex        MutexQueue: a lock, and a thread that has to wait sleeps
//   mutex-spin   MutexQueue behind SpinChannel: the same lock, but a thread
//                that has to wait spins
//   ring         SpscRing behind SpinChannel: no lock, and spinning
//
// mutex against mutex-spin is the cost of sleeping. mutex-spin against ring is
// the cost of the lock.

namespace obe::util {

struct MutexChannelKind {
    static constexpr std::string_view kName = "mutex";
    static constexpr std::string_view kDescription =
        "std::mutex and two condition variables; waits by sleeping";
    template <class T>
    using Channel = MutexQueue<T>;
};

struct MutexSpinChannelKind {
    static constexpr std::string_view kName = "mutex-spin";
    static constexpr std::string_view kDescription = "std::mutex; waits by spinning";
    template <class T>
    using Channel = SpinChannel<MutexQueue<T>>;
};

struct RingChannelKind {
    static constexpr std::string_view kName = "ring";
    static constexpr std::string_view kDescription =
        "SpscRing: lock-free single-producer single-consumer ring; waits by spinning";
    template <class T>
    using Channel = SpinChannel<SpscRing<T>>;
};

static_assert(BoundedQueue<MutexQueue<int>>);
static_assert(BoundedQueue<SpscRing<int>>);
static_assert(Channel<MutexChannelKind::Channel<int>>);
static_assert(Channel<MutexSpinChannelKind::Channel<int>>);
static_assert(Channel<RingChannelKind::Channel<int>>);

// Calls f(std::type_identity<Kind>{}) once for every channel, the baseline
// first.
template <class F>
void for_each_channel(F&& f) {
    f(std::type_identity<MutexChannelKind>{});
    f(std::type_identity<MutexSpinChannelKind>{});
    f(std::type_identity<RingChannelKind>{});
}

// Calls f(std::type_identity<Kind>{}) for the channel called `name`. Returns
// false, calling nothing, if there is no such channel.
template <class F>
bool with_channel(std::string_view name, F&& f) {
    bool found = false;
    for_each_channel([&]<class Kind>(std::type_identity<Kind> tag) {
        if (!found && Kind::kName == name) {
            found = true;
            f(tag);
        }
    });
    return found;
}

}  // namespace obe::util
