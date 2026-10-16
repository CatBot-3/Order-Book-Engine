#pragma once

#include <cstddef>
#include <limits>
#include <new>
#include <vector>

// Pool<T>: storage for objects of one type, handed out one slot at a time from
// slabs the pool owns, and recycled through a free list. Once it is warm it
// never calls the general-purpose allocator.
//
// Phase 4, experiment 4 (and the matching engine's orders in phase 5).
//
// The hypothesis (spec, phase 4): general-purpose allocation has an
// unpredictable cost, and what a trading system cares about is the tail. A
// pool makes allocate() and deallocate() a couple of pointer moves, and keeps
// objects of one kind next to each other.
//
// How it is used: obe/util/pool_allocator.hpp adapts it to the standard
// allocator interface, and obe/book/pooled.hpp gives the reference containers
// that allocator. That is the "pooled" book: the same std::unordered_map and
// std::map, with their nodes coming from pools. It separates "how much of the
// reference's cost was the allocator?" from "how much was the node-based
// layout?", which the flat table answers. The matching engine takes its order
// nodes from a Pool directly.
//
// The judge is tests/util/pool_test.cpp.
//
// DESIGN
//
//  1. The free list lives inside the free slots.
//     A free slot holds no object, so its bytes are free to use: the first
//     bytes of a free slot are the pointer to the next free slot. That needs
//     every slot to be at least as large as a pointer and aligned for one, as
//     well as for T. So a slot is max(sizeof(T), sizeof(void*)) rounded up to
//     max(alignof(T), alignof(void*)). A one-byte T costs eight bytes a slot;
//     a page-aligned T costs a page. The slab itself is allocated with that
//     alignment (aligned operator new), so every slot in it is aligned too.
//
//  2. Growing without moving.
//     Handed-out pointers must stay valid, so storage is never reallocated:
//     the pool keeps a list of slabs and adds one when the free list is
//     empty. All slabs are the same size, chosen by whoever constructs the
//     pool (objects_per_slab), because only the user knows whether this pool
//     will hold ten objects or ten million. Doubling the slab each time would
//     save a few calls to the allocator at the price of a large slab arriving
//     at an unpredictable moment; a fixed size keeps the cost of a refill the
//     same every time.
//
//  3. Starts empty.
//     The constructor allocates nothing. The first slab arrives with the
//     first allocate(). A pool inside an object made by the tens of thousands
//     costs three words until it is used.
//
//  4. Last freed, first reused.
//     The free list is a stack (LIFO). The slot handed out next is the one
//     freed most recently, which is the slot most likely to still be in the
//     cache. A new slab is threaded so that its slots come out in address
//     order. Over a long run with churn, addresses stop being in any order
//     (objects that live next to each other in time are no longer next to
//     each other in memory); handing out in address order would need a
//     sorted free structure and lose the cache-warm reuse, which is the
//     point.
//
//  5. Raw storage.
//     allocate() returns memory, not a constructed T; deallocate() runs no
//     destructor. The caller constructs with placement new and destroys
//     explicitly, which is exactly the split the standard allocator interface
//     wants. The pool's destructor frees the slabs and nothing else: an
//     object still alive in it is the caller's bug.
//
//  6. Deliberately not done: thread safety (one writer per book), giving
//     memory back to the system before destruction, detecting a double free.
//     A debug build could catch a double free by writing a marker into a slot
//     when it is freed and checking for it on the next free of the same slot;
//     that costs a store and a compare on every call, which is why it is not
//     here. AddressSanitizer cannot see such a bug either: to it, the slab is
//     one live allocation.

namespace obe::util {

template <class T>
class Pool {
 public:
    // `objects_per_slab` is how many T one slab holds (at least 1; 0 is taken
    // as 1). Allocates nothing until the first allocate().
    explicit Pool(std::size_t objects_per_slab = 4096)
        : per_slab_(objects_per_slab == 0 ? 1 : objects_per_slab) {}

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Frees every slab. Does not run any T destructor.
    ~Pool() {
        for (void* slab : slabs_) {
            ::operator delete(slab, std::align_val_t{kSlotAlign});
        }
    }

    // Storage for one T: sizeof(T) bytes aligned to alignof(T). Never null;
    // throws std::bad_alloc if the system is out of memory. The pointer stays
    // valid until it is passed to deallocate() or the pool is destroyed.
    [[nodiscard]] void* allocate() {
        if (free_ == nullptr) [[unlikely]] {
            grow();
        }
        FreeSlot* slot = free_;
        free_ = slot->next;
        ++in_use_;
        return slot;
    }

    // Returns storage obtained from allocate() on this pool. Must not throw.
    void deallocate(void* p) noexcept {
        auto* slot = static_cast<FreeSlot*>(p);
        slot->next = free_;
        free_ = slot;
        --in_use_;
    }

    // Slots currently handed out.
    [[nodiscard]] std::size_t in_use() const noexcept { return in_use_; }

    // Slots in all slabs, handed out or free.
    [[nodiscard]] std::size_t capacity() const noexcept { return slabs_.size() * per_slab_; }

 private:
    // What a free slot holds.
    struct FreeSlot {
        FreeSlot* next;
    };

    static constexpr std::size_t max(std::size_t a, std::size_t b) noexcept {
        return a > b ? a : b;
    }
    static constexpr std::size_t kSlotAlign = max(alignof(T), alignof(FreeSlot));
    // Rounding up to kSlotAlign, which is at least a pointer's alignment,
    // would already make room for the link; the max says why it must. (T can
    // be a pointer: a container rebinds its allocator to pointer types too.)
    // NOLINTBEGIN(bugprone-sizeof-expression)
    static constexpr std::size_t kSlotSize =
        (max(sizeof(T), sizeof(FreeSlot)) + kSlotAlign - 1) / kSlotAlign * kSlotAlign;
    // NOLINTEND(bugprone-sizeof-expression)

    // Adds a slab and threads all its slots onto the free list, lowest
    // address on top, so a fresh slab is handed out in address order.
    void grow() {
        // A slab size that does not fit in a size_t would wrap to a small
        // allocation, and the loop below would then write far past its end.
        if (per_slab_ > std::numeric_limits<std::size_t>::max() / kSlotSize) {
            throw std::bad_alloc();
        }
        // Room for the new slab's entry first: if this throws, nothing leaks.
        // Doubling, because reserve(size + 1) would copy the list on every slab.
        if (slabs_.size() == slabs_.capacity()) {
            slabs_.reserve(slabs_.empty() ? 8 : slabs_.size() * 2);
        }
        auto* slab = static_cast<std::byte*>(
            ::operator new(kSlotSize * per_slab_, std::align_val_t{kSlotAlign}));
        slabs_.push_back(slab);
        for (std::size_t i = per_slab_; i > 0; --i) {
            auto* slot = ::new (slab + (i - 1) * kSlotSize) FreeSlot{free_};
            free_ = slot;
        }
    }

    std::vector<void*> slabs_;
    FreeSlot* free_ = nullptr;
    std::size_t per_slab_;
    std::size_t in_use_ = 0;
};

}  // namespace obe::util
