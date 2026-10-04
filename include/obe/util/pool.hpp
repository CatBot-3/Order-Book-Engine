#pragma once

#include <cstddef>

#include "obe/util/todo.hpp"

// ============================================================================
//  YOURS TO WRITE: phase 4, experiment 4 (and the engine's orders in phase 5)
// ============================================================================
//
// Pool<T> hands out raw storage for objects of one type, one at a time, from
// slabs it owns, and recycles freed storage through a free list. It never calls
// the general-purpose allocator on the hot path once it is warm.
//
// The hypothesis (spec, phase 4): general-purpose allocation has an
// unpredictable cost, and what a trading system cares about is the tail. A
// pool makes allocate() and deallocate() a couple of pointer moves, and keeps
// objects of one kind next to each other.
//
// How it is used in phase 4: obe/util/pool_allocator.hpp adapts it to the
// standard allocator interface, and obe/book/pooled.hpp gives the reference
// containers that allocator. That is the "pooled" implementation: the same
// std::unordered_map and std::map, with their nodes coming from pools. It
// isolates the question "how much of the reference's cost was the allocator?"
// from the question "how much was the node-based layout?", which the flat
// table answers. Comparing the two is a better log entry than either alone.
//
// The judge is tests/util/pool_test.cpp. allocate() throws until you replace
// its body.
//
// Decisions to make:
//
//  1. Where does the free list live?
//     A free slot holds no object, so its bytes are yours: the link to the
//     next free slot can be stored inside the slot itself. What does that
//     require of the slot's size and alignment when T is smaller than a
//     pointer, or has stricter alignment than one? (The tests use a one-byte
//     T, a 64-byte-aligned T and a page-aligned T.)
//
//  2. Growing without moving.
//     Pointers the pool has handed out must stay valid forever, so the pool
//     cannot be one std::vector<T> that reallocates. It owns a list of slabs
//     and adds another when the free list runs dry. How big should a slab be,
//     and who should decide?
//
//  3. Start empty.
//     The pooled book creates one pool per node type, but a pool may also sit
//     inside objects that are created by the tens of thousands. A constructor
//     that allocates nothing, and a first slab on first use, keeps an unused
//     pool free.
//
//  4. Last freed, first reused?
//     A LIFO free list hands back the slot most likely to still be in cache.
//     What does that do to the addresses of objects over a long run, compared
//     with handing slots out in address order?
//
//  5. This is raw storage.
//     allocate() returns memory, not a constructed T, and deallocate() does
//     not run a destructor. The standard allocator interface wants exactly
//     that split. The pool's own destructor frees the slabs and nothing else.
//
//  6. What it deliberately does not do: no thread safety (one writer per
//     book), no return of memory to the system before destruction, no
//     detection of a double free. Which of those would you add for a debug
//     build, and how would you keep it out of the release hot path?

namespace obe::util {

template <class T>
class Pool {
 public:
    // `objects_per_slab` is how many T one slab holds. Must be at least 1.
    // Allocates nothing until the first allocate().
    explicit Pool(std::size_t objects_per_slab = 4096) { static_cast<void>(objects_per_slab); }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Frees every slab. Does not run any T destructor.
    ~Pool() = default;

    // Storage for one T: sizeof(T) bytes aligned to alignof(T). Never null;
    // throws std::bad_alloc if the system is out of memory. The pointer stays
    // valid until it is passed to deallocate() or the pool is destroyed.
    [[nodiscard]] void* allocate() { todo("Pool::allocate"); }

    // Returns storage obtained from allocate() on this pool. Must not throw.
    void deallocate(void* p) noexcept { static_cast<void>(p); }

    // Slots currently handed out.
    [[nodiscard]] std::size_t in_use() const noexcept { return 0; }

    // Slots in all slabs, handed out or free.
    [[nodiscard]] std::size_t capacity() const noexcept { return 0; }

 private:
    // Your storage goes here.
};

}  // namespace obe::util
