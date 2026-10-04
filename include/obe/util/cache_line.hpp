#pragma once

#include <cstddef>

namespace obe::util {

// The size of a cache line, for keeping two variables that different threads
// write off the same line.
//
// When one core writes to a line, every other core's copy of that line is
// invalidated, whether or not they care about the bytes that changed. Two
// counters that sit next to each other and are written by two threads
// therefore fight over one line: "false sharing". Aligning each to a line of
// its own stops it.
//
// C++17 has std::hardware_destructive_interference_size for this. It is not
// used here because GCC warns when it appears in a header: its value depends
// on tuning flags, so two translation units built differently could disagree
// about a struct's layout. A fixed 64 is right for every x86-64 CPU and for
// most 64-bit ARM cores (Apple's M-series uses 128; on those this would be a
// performance bug, never a correctness one).
inline constexpr std::size_t kCacheLine = 64;

}  // namespace obe::util
