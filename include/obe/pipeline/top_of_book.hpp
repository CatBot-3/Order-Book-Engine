#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "obe/book/types.hpp"
#include "obe/types.hpp"
#include "obe/util/seqlock.hpp"

// The latest best bid and offer of every security, readable from any thread.
//
// The queue to the consumer thread carries every update, in order, to one
// reader. This board answers a different question for any number of readers:
// "what is the top of book for this security right now?". A strategy thread,
// a monitoring page or a risk check wants that, and none of them should be
// able to slow the book thread down by asking.
//
// One Seqlock per locate. The book thread publishes; readers take a
// consistent snapshot or retry. The writer's cost is the same whether there
// are no readers or a hundred.

namespace obe::pipeline {

class TopOfBookBoard {
 public:
    static constexpr std::size_t kLocates = std::size_t{1} << 16;

    TopOfBookBoard() : slots_(std::make_unique<Slot[]>(kLocates)) {}

    // Book thread only.
    void publish(const book::BboUpdate& update) noexcept { slots_[update.locate].store(update); }

    // Any thread. The last update published for the locate. Before the first
    // one, an update with every field zero.
    [[nodiscard]] book::BboUpdate read(Locate locate) const noexcept {
        return slots_[locate].load();
    }

    // One attempt, for a reader that would sooner skip than wait.
    [[nodiscard]] bool try_read(Locate locate, book::BboUpdate& out) const noexcept {
        return slots_[locate].try_load(out);
    }

    // How many times this locate has been published.
    [[nodiscard]] std::uint64_t updates(Locate locate) const noexcept {
        return slots_[locate].version() - 1;
    }

 private:
    using Slot = util::Seqlock<book::BboUpdate>;

    // A plain array, not a vector: a Seqlock holds atomics and cannot be
    // copied or moved, which a vector's element type must allow.
    std::unique_ptr<Slot[]> slots_;
};

}  // namespace obe::pipeline
