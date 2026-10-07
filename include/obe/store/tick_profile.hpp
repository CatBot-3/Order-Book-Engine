#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/store/tick_codec.hpp"
#include "obe/types.hpp"

// What consecutive ticks differ by.
//
// A compressing codec is a bet about its input: that times are close
// together, that the same securities come round again, that a quote changes
// in one place at a time and by a little. TickProfile measures each of those
// on a real stream, so that a codec can be designed from the numbers
// (`tick_store profile <ITCH file>` prints them).
//
// It measures and nothing else. It does not say what to do about any of it.

namespace obe::store {

// Values counted by how large they are. The classes are powers of 128, which
// is what one more byte of a variable-length integer buys (util/varint.hpp):
//
//   [0] exactly 0        [3] below 128^3 = 2,097,152
//   [1] below 128        [4] below 128^4 = 268,435,456
//   [2] below 16,384     [5] anything larger
struct Magnitudes {
    static constexpr std::size_t kClasses = 6;
    std::array<std::uint64_t, kClasses> count{};

    [[nodiscard]] static constexpr std::size_t class_of(std::uint64_t v) noexcept {
        if (v == 0) {
            return 0;
        }
        std::size_t k = 1;
        while (k < kClasses - 1 && v >= 128) {
            v /= 128;
            ++k;
        }
        return k;
    }

    constexpr void add(std::uint64_t v) noexcept { ++count[class_of(v)]; }

    [[nodiscard]] constexpr std::uint64_t total() const noexcept {
        std::uint64_t sum = 0;
        for (const std::uint64_t c : count) {
            sum += c;
        }
        return sum;
    }

    friend bool operator==(const Magnitudes&, const Magnitudes&) = default;
};

// How one field of the quote changed, counted only on the ticks where it did,
// against the same security's tick before.
struct FieldChanges {
    std::uint64_t changes = 0;
    std::uint64_t down = 0;             // the new value was the smaller
    std::uint64_t to_or_from_zero = 0;  // a side appearing or emptying
    std::uint64_t hundreds = 0;         // the difference was a multiple of 100:
                                        // whole cents for a price, round lots for a size
    std::uint64_t one_hundred = 0;      // it was exactly 100: one cent, one round lot
    Magnitudes size;                    // of the difference, whichever way it went

    constexpr void add(std::uint64_t before, std::uint64_t now) noexcept {
        if (before == now) {
            return;
        }
        const std::uint64_t by = now > before ? now - before : before - now;
        ++changes;
        down += now < before ? 1U : 0U;
        to_or_from_zero += (before == 0 || now == 0) ? 1U : 0U;
        hundreds += by % 100 == 0 ? 1U : 0U;
        one_hundred += by == 100 ? 1U : 0U;
        size.add(by);
    }

    friend bool operator==(const FieldChanges&, const FieldChanges&) = default;
};

class TickProfile {
 public:
    // Bits of a change mask: which of the four fields of the quote differ
    // from the same security's tick before.
    static constexpr unsigned kBidPrice = 1;
    static constexpr unsigned kBidQty = 2;
    static constexpr unsigned kAskPrice = 4;
    static constexpr unsigned kAskQty = 8;

    TickProfile() : last_(std::size_t{1} << 16), seen_at_(std::size_t{1} << 16, 0) {}

    void add(const Tick& tick) {
        ++ticks_;
        if (ticks_ > 1) {
            if (tick.timestamp < prev_time_) {
                ++time_backwards_;
                time_gap_.add(prev_time_ - tick.timestamp);
            } else {
                time_gap_.add(tick.timestamp - prev_time_);
            }
            same_security_ += tick.locate == prev_locate_ ? 1U : 0U;
        }
        prev_time_ = tick.timestamp;
        prev_locate_ = tick.locate;

        std::uint64_t& seen_at = seen_at_[tick.locate];
        book::Bbo& last = last_[tick.locate];
        if (seen_at == 0) {
            ++securities_;
        } else {
            ticks_since_.add(ticks_ - seen_at);
            unsigned mask = 0;
            mask |= tick.bbo.bid_price != last.bid_price ? kBidPrice : 0U;
            mask |= tick.bbo.bid_qty != last.bid_qty ? kBidQty : 0U;
            mask |= tick.bbo.ask_price != last.ask_price ? kAskPrice : 0U;
            mask |= tick.bbo.ask_qty != last.ask_qty ? kAskQty : 0U;
            ++masks_[mask];
            bid_price_.add(last.bid_price, tick.bbo.bid_price);
            bid_qty_.add(last.bid_qty, tick.bbo.bid_qty);
            ask_price_.add(last.ask_price, tick.bbo.ask_price);
            ask_qty_.add(last.ask_qty, tick.bbo.ask_qty);
        }
        seen_at = ticks_;
        last = tick.bbo;
    }

    // So that it can be given to a book as its listener.
    void on_bbo(const book::BboUpdate& update) { add(update); }

    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }
    [[nodiscard]] std::uint64_t securities() const noexcept { return securities_; }

    // Against the tick just before in the stream, whatever its security.
    // Counted from the second tick on.
    [[nodiscard]] const Magnitudes& time_gap() const noexcept { return time_gap_; }
    [[nodiscard]] std::uint64_t time_backwards() const noexcept { return time_backwards_; }
    [[nodiscard]] std::uint64_t same_security() const noexcept { return same_security_; }

    // Against the same security's tick before. Counted on every tick but a
    // security's first.
    //
    // How many ticks ago that was: 1 is the tick just before.
    [[nodiscard]] const Magnitudes& ticks_since() const noexcept { return ticks_since_; }
    // How many ticks changed exactly this set of fields. masks()[0] counts
    // ticks that repeat the quote before.
    [[nodiscard]] const std::array<std::uint64_t, 16>& masks() const noexcept { return masks_; }
    [[nodiscard]] const FieldChanges& bid_price() const noexcept { return bid_price_; }
    [[nodiscard]] const FieldChanges& bid_qty() const noexcept { return bid_qty_; }
    [[nodiscard]] const FieldChanges& ask_price() const noexcept { return ask_price_; }
    [[nodiscard]] const FieldChanges& ask_qty() const noexcept { return ask_qty_; }

 private:
    std::vector<book::Bbo> last_;         // by locate: the quote of its last tick
    std::vector<std::uint64_t> seen_at_;  // by locate: which tick that was, 0 for never
    std::uint64_t ticks_ = 0;
    std::uint64_t securities_ = 0;
    Nanos prev_time_ = 0;
    Locate prev_locate_ = 0;
    Magnitudes time_gap_;
    std::uint64_t time_backwards_ = 0;
    std::uint64_t same_security_ = 0;
    Magnitudes ticks_since_;
    std::array<std::uint64_t, 16> masks_{};
    FieldChanges bid_price_;
    FieldChanges bid_qty_;
    FieldChanges ask_price_;
    FieldChanges ask_qty_;
};

}  // namespace obe::store
