#pragma once

#include <cstdint>

namespace obe {

// Order reference number. Nasdaq assigns it; a replace issues a new one.
using OrderId = std::uint64_t;

// Prices are fixed-point integers with four implied decimal places, exactly as
// they arrive on the wire: 1'234'500 is $123.45. They are never converted to
// floating point inside the engine. Formatting happens at the display edge
// (obe/util/format.hpp).
using Price = std::uint32_t;
inline constexpr Price kPriceScale = 10'000;

// A few fields (the market-wide circuit breaker levels) use eight decimals.
using Price8 = std::uint64_t;
inline constexpr Price8 kPrice8Scale = 100'000'000;

using Qty = std::uint32_t;

// Stock locate: a small integer that identifies a security for one day only.
// Never hard-code one. The mapping comes from that day's Stock Directory
// messages.
using Locate = std::uint16_t;

// Nanoseconds since midnight. 48 bits on the wire.
using Nanos = std::uint64_t;

// The enumerator values are the wire bytes, so decoding is a cast. A corrupt
// byte yields a value that is neither enumerator; that is well defined for an
// enum with a fixed underlying type, and callers that care must check.
enum class Side : char { Buy = 'B', Sell = 'S' };

}  // namespace obe
