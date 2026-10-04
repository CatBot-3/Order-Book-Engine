#pragma once

#include <cstddef>
#include <new>
#include <type_traits>

#include "obe/util/pool.hpp"

// Adapts Pool<T> to the standard allocator interface, so a node-based standard
// container can take its nodes from a pool.
//
// How a container uses it. std::map<K, V, Cmp, PoolAllocator<pair<const K, V>>>
// never allocates a pair. It rebinds the allocator to its private node type
// and asks for one node at a time. Those single-object requests are the ones
// that go to the pool. Anything else (the bucket array of an unordered_map is
// a request for many objects at once) goes to operator new, because a pool of
// fixed-size slots cannot serve it.
//
// One pool per node type, shared by every container. The allocator is
// stateless: all instances use the same function-local Pool<T>. The
// alternative, a pool per container, would give each of the 131072 price-level
// maps in a BookManager its own slabs, which is gigabytes of mostly empty
// memory. Sharing also means a node freed by one book is reused by the next
// book that needs one.
//
// Consequences worth knowing:
//   - Not thread-safe. Two threads using pooled containers of the same node
//     type race on the shared free list. The single-writer design (one thread
//     owns all books) is what makes this acceptable.
//   - The pool lives until the program exits and never returns memory before
//     that, so its footprint is the high-water mark.

namespace obe::util {

template <class T>
class PoolAllocator {
 public:
    using value_type = T;
    using is_always_equal = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    PoolAllocator() noexcept = default;

    // Rebinding: a container converts PoolAllocator<pair<...>> into
    // PoolAllocator<its node type>. There is no state to carry over.
    // Implicit on purpose: the standard requires this conversion.
    template <class U>
    PoolAllocator(const PoolAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n == 1) {
            return static_cast<T*>(pool().allocate());
        }
        // T can itself be a pointer here (an unordered_map's bucket array is an
        // array of node pointers), which is what the lint check below objects
        // to. The size of T is exactly what is wanted.
        // NOLINTNEXTLINE(bugprone-sizeof-expression)
        return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t{alignof(T)}));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        // When T is a pointer type, T* is a pointer to a pointer, and the lint
        // check flags its conversion to void*. Handing raw storage back is
        // exactly such a conversion.
        // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion)
        void* const raw = static_cast<void*>(p);
        if (n == 1) {
            pool().deallocate(raw);
        } else {
            ::operator delete(raw, std::align_val_t{alignof(T)});
        }
    }

    // The pool behind every PoolAllocator<T>. Exposed so tests and tools can
    // read its counters.
    [[nodiscard]] static Pool<T>& pool() {
        static Pool<T> instance;
        return instance;
    }
};

// Stateless, so any two compare equal: memory from one can be freed by another.
template <class T, class U>
[[nodiscard]] bool operator==(const PoolAllocator<T>&, const PoolAllocator<U>&) noexcept {
    return true;
}

}  // namespace obe::util
