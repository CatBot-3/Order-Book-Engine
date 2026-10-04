#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/types.hpp"

// A listener that folds every best-bid-and-offer update into a running hash,
// one per security.
//
// This is the instrument for success criterion 3. Run the reference book and
// an optimized book over the same day, each with a BboHasher. If the two agree
// for a security, both books published the same sequence of updates for it:
// the same prices and sizes, at the same timestamps, in the same order. Any
// difference in any update changes the hash from that point on.
//
// It is also the determinism check: the same input must give the same hashes
// on every run.

namespace obe::book {

class BboHasher {
 public:
    static constexpr std::size_t kLocates = std::size_t{1} << 16;

    BboHasher() : hashes_(kLocates, kSeed), events_(kLocates, 0) {}

    void on_bbo(const BboUpdate& u) noexcept {
        std::uint64_t h = hashes_[u.locate];
        h = mix(h, u.timestamp);
        h = mix(h, u.bbo.bid_price);
        h = mix(h, u.bbo.bid_qty);
        h = mix(h, u.bbo.ask_price);
        h = mix(h, u.bbo.ask_qty);
        hashes_[u.locate] = h;
        ++events_[u.locate];
        ++total_events_;
    }

    // Hash of every update published for this security so far.
    [[nodiscard]] std::uint64_t hash(Locate locate) const noexcept { return hashes_[locate]; }
    [[nodiscard]] std::uint64_t events(Locate locate) const noexcept { return events_[locate]; }
    [[nodiscard]] std::uint64_t total_events() const noexcept { return total_events_; }

    // One number for the whole run: every security that published anything,
    // in locate order.
    [[nodiscard]] std::uint64_t combined() const noexcept {
        std::uint64_t h = kSeed;
        for (std::size_t i = 0; i < kLocates; ++i) {
            if (events_[i] != 0) {
                h = mix(h, i);
                h = mix(h, events_[i]);
                h = mix(h, hashes_[i]);
            }
        }
        return h;
    }

 private:
    // FNV-1a, applied a 64-bit word at a time. It only has to make an
    // accidental match between two different update streams implausible; it is
    // not defending against anyone.
    static constexpr std::uint64_t kSeed = 0xcbf29ce484222325ULL;
    static constexpr std::uint64_t kPrime = 0x100000001b3ULL;

    [[nodiscard]] static constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t word) noexcept {
        h ^= word;
        h *= kPrime;
        // Fold the high bits down so that every bit of `word` reaches every
        // bit of the state within a couple of steps.
        return h ^ (h >> 32);
    }

    std::vector<std::uint64_t> hashes_;
    std::vector<std::uint64_t> events_;
    std::uint64_t total_events_ = 0;
};

}  // namespace obe::book
