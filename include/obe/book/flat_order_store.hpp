#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "obe/book/concepts.hpp"
#include "obe/book/types.hpp"
#include "obe/types.hpp"

// FlatOrderStore: the optimized order store. An open-addressing hash table
// that keeps every order in one contiguous array of slots, instead of one heap
// node per order. Phase 4, experiment 1.
//
// The hypothesis (spec, phase 4): std::unordered_map allocates a node per order
// and chases two pointers per lookup; a flat table removes the allocation and
// turns most lookups into one cache line. docs/optimization-log.md has the
// entry with the measured result.
//
// The contract is OrderStoreLike (obe/book/concepts.hpp), the same one the
// reference satisfies, so BookManager and every test work unchanged. The judges:
//
//   tests/book/order_store_test.cpp     the container contract and a model test
//   tests/book/differential_test.cpp    the whole book against the reference
//   scripts/diff_books.sh               the same check on a full real day
//   replay_bench --impl flat-store      refuses to report timings as valid
//                                       unless the hash matches the reference
//
// DESIGN
//
//  1. Collisions: linear probing.
//     An order whose home slot is taken goes in the next free slot. A probe
//     then walks consecutive memory, usually inside one or two cache lines,
//     and the hardware prefetcher follows it. Quadratic probing and double
//     hashing break up clusters but jump around memory to do it; Robin Hood
//     bounds the worst probe by moving entries on every insert. This table's
//     traffic is nearly all find() calls that hit, and a short probe from a
//     good hash at a moderate load (point 5) is already one or two slots:
//     the simplest scheme with the best locality wins.
//
//  2. Deletion: backward shift, no tombstones.
//     An emptied slot cannot simply be marked free: a find() for a key that
//     was placed further along, because this slot was taken when it was
//     inserted, would stop here and miss it. Tombstones ("deleted, keep
//     going") fix that, but an ITCH day adds and removes hundreds of millions
//     of orders at a roughly constant size. Tombstones accumulate, every probe
//     walks over them, and without a periodic rebuild the table ends up full
//     of them. Instead erase() pulls later entries of the same cluster back
//     into the hole: walking on from the hole, each entry whose home slot is
//     not between the hole and itself (cyclically) moves into the hole, and
//     its old slot becomes the new hole, until an empty slot is reached. The
//     table is then exactly as if the erased key had never been inserted, so
//     probe lengths do not decay with churn.
//
//  3. Empty or used: one byte, in padding that is there anyway.
//     A reserved key would be cheapest, but reference numbers 0 and 2^64 - 1
//     are both legal (and tested). A slot is {8-byte key, 12-byte record}: 20
//     bytes, which the compiler pads to 24 for the key's alignment. A flag
//     byte fits in those 4 spare bytes, so marking occupancy costs nothing in
//     size, and it sits in the same line as the key it describes.
//
//  4. The hash: Fibonacci hashing, the top bits of key * 2^64/phi.
//     The identity function with a mask would put consecutive reference
//     numbers in consecutive slots, which looks ideal for sequential ITCH
//     numbers but makes long runs of occupied slots out of them, and it
//     collapses keys that differ only in their high bits into one slot (the
//     test KeysThatDifferOnlyInTheirHighBitsStayDistinct). One multiply by
//     the 64-bit golden-ratio constant, keeping the top log2(capacity) bits,
//     spreads any pattern of keys evenly, because every input bit reaches the
//     top bits of the product. It costs one multiply and a shift.
//
//  5. Capacity and growth: a power of two, at most 70% full.
//     A power-of-two capacity makes the slot index a shift (and a mask for
//     the probe). Linear probing's expected probe length grows like
//     1/(1 - load)^2 for a miss; at 70% that is about 6 slots for a miss and
//     2 for a hit, with misses rare here. Past the limit the table doubles and
//     every entry is re-inserted, which is why a pointer from find() dies at
//     the next insert or erase. Pre-sized from feed_profile's peak of resting
//     orders (replay_bench --reserve), a real day never grows.
//
//  6. Layout: an array of {key, record, flag} slots (experiment 5).
//     A probe compares keys, and a hit then reads the record next to the key
//     it matched: one line for both. Keys in a separate array would put eight
//     keys in a line instead of two and a half, and make every hit touch a
//     second array. With probes of one or two slots, the record next door
//     wins; a separate-arrays layout would pay off for long probes, which
//     point 5 avoids.
//
//  7. Movable: BookManager takes its store by value. The move operations leave
//     the source an empty table that still works.
//
//  8. prefetch(id) (experiment 6): replay_bench --prefetch calls it for the
//     order the next message refers to, before handling the current one. It
//     issues a software prefetch of that key's home slot.

namespace obe::book {

class FlatOrderStore {
 public:
    FlatOrderStore() : FlatOrderStore(std::size_t{0}) {}

    // Pre-size for `expected_orders` live orders.
    explicit FlatOrderStore(std::size_t expected_orders) {
        std::size_t capacity = kMinCapacity;
        while (capacity * kLoadNumerator / kLoadDenominator < expected_orders) {
            capacity *= 2;
        }
        reset(capacity);
    }

    FlatOrderStore(const FlatOrderStore&) = default;
    FlatOrderStore& operator=(const FlatOrderStore&) = default;

    FlatOrderStore(FlatOrderStore&& other) noexcept
        : slots_(std::move(other.slots_)),
          mask_(std::exchange(other.mask_, 0)),
          shift_(std::exchange(other.shift_, 0)),
          size_(std::exchange(other.size_, 0)) {
        other.slots_.clear();
    }

    FlatOrderStore& operator=(FlatOrderStore&& other) noexcept {
        if (this != &other) {
            slots_ = std::move(other.slots_);
            other.slots_.clear();
            mask_ = std::exchange(other.mask_, 0);
            shift_ = std::exchange(other.shift_, 0);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~FlatOrderStore() = default;

    // Adds the order. Returns false, changing nothing, if `id` is present.
    bool insert(OrderId id, const OrderRecord& rec) {
        if ((size_ + 1) * kLoadDenominator > slots_.size() * kLoadNumerator) [[unlikely]] {
            grow();
        }
        std::size_t i = home(id);
        while (slots_[i].used) {
            if (slots_[i].key == id) {
                return false;
            }
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{id, rec, true};
        ++size_;
        return true;
    }

    // The stored record, or nullptr. Valid until the next insert or erase.
    OrderRecord* find(OrderId id) noexcept {
        const std::size_t i = locate(id);
        return i == kNone ? nullptr : &slots_[i].rec;
    }
    const OrderRecord* find(OrderId id) const noexcept {
        const std::size_t i = locate(id);
        return i == kNone ? nullptr : &slots_[i].rec;
    }

    // Removes the order. Returns false if it was not present.
    bool erase(OrderId id) noexcept {
        std::size_t hole = locate(id);
        if (hole == kNone) {
            return false;
        }
        // Backward shift (design point 2): walk the rest of the cluster and
        // pull back every entry the hole would otherwise cut off from its
        // home slot.
        for (std::size_t j = (hole + 1) & mask_; slots_[j].used; j = (j + 1) & mask_) {
            const std::size_t k = home(slots_[j].key);
            // Entry j may stay where it is if its home k lies cyclically in
            // (hole, j]: a probe from k reaches j without passing the hole.
            const bool stays = hole <= j ? (hole < k && k <= j) : (hole < k || k <= j);
            if (!stays) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].used = false;
        --size_;
        return true;
    }

    // Number of orders currently stored.
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Slots in the table, used or not. For tests and reports.
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    // Experiment 6: start loading the slot `id` hashes to.
    void prefetch(OrderId id) const noexcept {
        if (!slots_.empty()) {
            __builtin_prefetch(slots_.data() + home(id));
        }
    }

 private:
    struct Slot {
        OrderId key = 0;
        OrderRecord rec{};
        bool used = false;  // lives in the slot's padding (design point 3)
    };
    static_assert(sizeof(Slot) == 24, "the flag was meant to fit in the padding");

    static constexpr std::size_t kMinCapacity = 16;
    static constexpr std::size_t kLoadNumerator = 7;  // at most 7/10 full
    static constexpr std::size_t kLoadDenominator = 10;
    static constexpr std::size_t kNone = ~std::size_t{0};
    static constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ULL;  // 2^64 / phi

    [[nodiscard]] std::size_t home(OrderId id) const noexcept {
        return static_cast<std::size_t>((id * kGolden) >> shift_);
    }

    // The slot holding `id`, or kNone.
    [[nodiscard]] std::size_t locate(OrderId id) const noexcept {
        if (size_ == 0) {
            return kNone;  // also covers a moved-from table, which has no slots
        }
        std::size_t i = home(id);
        while (slots_[i].used) {
            if (slots_[i].key == id) {
                return i;
            }
            i = (i + 1) & mask_;
        }
        return kNone;
    }

    void reset(std::size_t capacity) {
        slots_.assign(capacity, Slot{});
        mask_ = capacity - 1;
        shift_ = 64U - static_cast<unsigned>(std::countr_zero(capacity));
        size_ = 0;
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        reset(old.empty() ? kMinCapacity : old.size() * 2);
        for (const Slot& s : old) {
            if (s.used) {
                std::size_t i = home(s.key);
                while (slots_[i].used) {
                    i = (i + 1) & mask_;
                }
                slots_[i] = s;
                ++size_;
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 0;
    std::size_t size_ = 0;
};

static_assert(OrderStoreLike<FlatOrderStore>);

}  // namespace obe::book
