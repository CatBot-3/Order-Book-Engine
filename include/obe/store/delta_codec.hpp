#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "obe/store/tick_codec.hpp"
#include "obe/util/todo.hpp"
#include "obe/util/varint.hpp"

// ============================================================================
//  YOURS TO WRITE: the compressing tick codec
// ============================================================================
//
// RawCodec writes every tick in 34 bytes. Almost all of those bytes say
// something the reader could have guessed: the time is a few microseconds
// after the last tick, the security is one that ticked recently, one side of
// its quote has not moved at all and the other moved by a cent or changed
// size by a round lot. DeltaCodec writes only what could not be guessed.
//
// The file format, the blocks, the index and the tools around them are done
// (tick_file.hpp, apps/tick_store.cpp). The three functions below throw until
// you write them. The contract is at the top of tick_codec.hpp; in short:
// lossless for EVERY tick, state forgotten at reset(), never a read at or
// past `end`, never more than kMaxTickSize bytes for one tick.
//
// The judges, labelled needs-your-code until this file is written:
//
//   tests/store/tick_codec_test.cpp   the contract, on hand-made and random
//                                     ticks, including the hostile ones
//   tests/store/tick_file_test.cpp    whole stores written and read back
//   tests/store/compression_test.cpp  a target: no more than 10 bytes a tick,
//                                     on average, over a generated market
//
//   ctest --preset debug -L needs-your-code -R 'Tick|Compression'
//
// util/varint.hpp has the two tools you are most likely to want (varints and
// zigzag) with their own tests. Whether to use them is your call.
//
// LOOK BEFORE YOU DESIGN
//
//   build/release/apps/tick_store profile <ITCH file>
//
// prints what consecutive ticks actually differ by: the gaps between
// timestamps, how often each field changes and by how much, how many ticks
// pass before a security ticks again. A codec is a bet about that table.
// Design from the table, not from intuition, and put the table in the log
// next to the result.
//
// Decisions to make:
//
//  1. What is "the previous tick"?
//     The one just before in the file, or the last one for the same
//     security? Which fields does each choice help? You can use both: a
//     different "previous" for different fields.
//
//  2. Per-security memory, and what reset() costs.
//     Remembering the last quote of every security takes an entry for each
//     of 65536 locates. reset() runs at the start of every block, and a block
//     may hold a few thousand ticks. If reset() clears the whole table, how
//     many bytes does it touch per tick written? What could you keep with
//     each entry so that reset() touches nothing at all? And a reader makes
//     one codec for every query it runs (TickReader::scan): what does
//     constructing yours cost, and does a query for one block notice?
//
//  3. The first tick of a security in a block.
//     There is no previous quote to subtract. What do you subtract from, and
//     does that special case need any code?
//
//  4. Signed differences of unsigned fields.
//     A share count is a 64-bit unsigned number and its difference from the
//     last one can be negative. What happens if you subtract the two as
//     unsigned numbers, let the result wrap, and add it back the same way on
//     the other side? Is anything lost, for any pair of values? Then which
//     wrapped differences are short as varints, and what does zigzag do
//     about the rest?
//
//  5. Saying what did not change.
//     Most ticks change one or two of the four quote fields. How does the
//     decoder learn which? If a byte is spent on that, are there bits left
//     over in it, and what is the most valuable thing they could say?
//
//  6. Prices move in ticks, and the tick is not 1.
//     Prices are in units of 1/10000 of a dollar and most stocks move in
//     cents: differences of 100, 200, 300. A varint holds 0 to 127 in one
//     byte. What would it take to get a one-cent move into one byte without
//     breaking the stocks that trade in sub-penny increments? Is it worth
//     the bits? (The profile says how common each case is.)
//
//  7. The worst case.
//     Add up the largest encoding your design can produce for one tick. It
//     must not exceed kMaxTickSize, and the writer relies on that to size its
//     buffer. If it does exceed it, change the design or the constant, not
//     the arithmetic.
//
//  8. Reading bytes you did not write.
//     decode() is handed bytes from a file that may be damaged. Every read
//     needs to know where `end` is. After it has returned 0 the reader gives
//     up on the block, so the state may be left in any condition, but
//     nothing may have been read out of bounds on the way. The tests run
//     under AddressSanitizer on buffers of exactly the right size to hold
//     you to that.
//
//  9. Against a general-purpose compressor.
//     `tick_store record --codec raw` writes fixed-width ticks; run `zstd -19`
//     or `xz` on that file and compare with yours. A general compressor knows
//     nothing about ticks and has a very large window; yours knows everything
//     and looks back one tick. Which wins, by how much, and at what speed
//     (`tick_bench`)? Whatever the answer, it goes in the log.
//
// Start with the simplest design that is correct, measure it, and let the
// profile say which of these questions is worth the next experiment. Each
// experiment is an entry in docs/optimization-log.md: bytes per tick and
// ticks per second, before and after.

namespace obe::store {

class DeltaCodec {
 public:
    static constexpr std::string_view kName = "delta";
    static constexpr std::string_view kDescription =
        "differences from the last tick, in variable-length integers; written by hand";
    static constexpr std::uint16_t kId = 2;
    // The most bytes one tick may take. Yours to revise with question 7.
    static constexpr std::size_t kMaxTickSize = 64;

    void reset() { util::todo("DeltaCodec::reset"); }

    std::size_t encode(const Tick& tick, std::byte* out) {
        util::todo("DeltaCodec::encode", tick, out);
    }

    [[nodiscard]] std::size_t decode(const std::byte* p, const std::byte* end, Tick& tick) {
        util::todo("DeltaCodec::decode", p, end, tick);
    }

 private:
    // Your state goes here.
};

static_assert(TickCodec<DeltaCodec>);

}  // namespace obe::store
