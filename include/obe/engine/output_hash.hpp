#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "obe/engine/concepts.hpp"
#include "obe/engine/types.hpp"

// Two running hashes over everything an engine says.
//
// Two engines given the same requests must produce the same market data and
// the same reports. In a test both outputs are kept and compared element by
// element. For a run of a hundred million requests that is too much to keep,
// so the outputs are folded into two numbers instead: if both numbers agree,
// the two engines said the same things in the same order.

namespace obe::engine {

namespace detail {
// FNV-1a. It only has to make an accidental match implausible.
inline constexpr std::uint64_t kHashSeed = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kHashPrime = 0x100000001b3ULL;

[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t h, std::uint64_t word) noexcept {
    h ^= word;
    h *= kHashPrime;
    return h ^ (h >> 32);
}
}  // namespace detail

// Hash of a byte stream, fed in pieces of any size.
class ByteHasher {
 public:
    void update(std::span<const std::byte> bytes) noexcept {
        std::uint64_t h = hash_;
        for (const std::byte b : bytes) {
            h ^= static_cast<std::uint64_t>(b);
            h *= detail::kHashPrime;
        }
        hash_ = h;
        bytes_ += bytes.size();
    }

    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

 private:
    std::uint64_t hash_ = detail::kHashSeed;
    std::uint64_t bytes_ = 0;
};

// A report sink that hashes every field of every report, in order.
class ReportHasher {
 public:
    void on_accepted(const Accepted& r) noexcept {
        tag('A');
        add(r.order_id);
        add(r.owner);
        add(r.token);
        add(r.locate);
        add(static_cast<unsigned char>(r.side));
        add(r.qty);
        add(r.price);
        add(static_cast<std::uint64_t>(r.kind));
        add(static_cast<std::uint64_t>(r.tif));
        add(r.timestamp);
    }
    void on_executed(const Executed& r) noexcept {
        tag('E');
        add(r.order_id);
        add(r.owner);
        add(r.token);
        add(r.qty);
        add(r.price);
        add(r.leaves);
        add(r.match_number);
        add(static_cast<unsigned char>(r.liquidity));
        add(r.timestamp);
    }
    void on_cancelled(const Cancelled& r) noexcept {
        tag('C');
        add(r.order_id);
        add(r.owner);
        add(r.token);
        add(r.qty);
        add(r.leaves);
        add(static_cast<std::uint64_t>(r.reason));
        add(r.timestamp);
    }
    void on_replaced(const Replaced& r) noexcept {
        tag('U');
        add(r.old_id);
        add(r.new_id);
        add(r.owner);
        add(r.token);
        add(r.qty);
        add(r.price);
        add(r.kept_priority ? 1U : 0U);
        add(r.timestamp);
    }
    void on_rejected(const Rejected& r) noexcept {
        tag('J');
        add(r.owner);
        add(r.token);
        add(r.order_id);
        add(static_cast<std::uint64_t>(r.reason));
        add(r.timestamp);
    }

    [[nodiscard]] std::uint64_t hash() const noexcept { return hash_; }
    [[nodiscard]] std::uint64_t reports() const noexcept { return reports_; }

 private:
    void tag(char kind) noexcept {
        ++reports_;
        add(static_cast<unsigned char>(kind));
    }
    void add(std::uint64_t word) noexcept { hash_ = detail::mix(hash_, word); }

    std::uint64_t hash_ = detail::kHashSeed;
    std::uint64_t reports_ = 0;
};

static_assert(ReportSink<ReportHasher>);

}  // namespace obe::engine
