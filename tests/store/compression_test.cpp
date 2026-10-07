#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

#include "obe/store/codecs.hpp"
#include "obe/store/tick_file.hpp"
#include "support/flow_run.hpp"
#include "support/tick_streams.hpp"

// The target for the hand-written codec. Correctness is judged elsewhere
// (tick_codec_test.cpp, tick_file_test.cpp): a codec that fails those is
// wrong, and how small its output is does not matter. This file asks only how
// small.
//
// The streams here are generated, so the numbers say that the codec does what
// a codec should on ticks shaped like a market. What it does on a real day is
// measured with `tick_store record` on a real file, and belongs in
// docs/optimization-log.md.
//
// Ten bytes a tick is a first target, not a good result: it is 3.4 times
// smaller than RawCodec's 34, and the plainest design that takes differences
// and writes them as varints gets under it. The questions in delta_codec.hpp
// are about how far below it you can go.

namespace {

using namespace obe;
using store::Tick;

struct Size {
    double payload_per_tick = 0;  // the codec's own bytes
    std::size_t file_bytes = 0;   // with block headers and the index
};

template <class Codec>
Size size_of(const std::vector<Tick>& ticks,
             std::uint32_t ticks_per_block = store::kDefaultTicksPerBlock) {
    std::vector<std::byte> file;
    test::MemoryTickWriter<Codec> writer(test::AppendTicksTo{&file}, ticks_per_block);
    for (const Tick& tick : ticks) {
        writer.append(tick);
    }
    writer.finish();
    return {static_cast<double>(writer.payload_bytes()) / static_cast<double>(ticks.size()),
            file.size()};
}

// Prints the measurement where a test run shows it, and returns it.
double measured(std::string_view what, const std::vector<Tick>& ticks) {
    const Size delta = size_of<store::DeltaCodec>(ticks);
    const Size raw = size_of<store::RawCodec>(ticks);
    std::cout << "[ measured ] " << what << ": " << std::fixed << std::setprecision(2)
              << delta.payload_per_tick << " bytes a tick over " << ticks.size()
              << " ticks; the file is " << std::setprecision(1)
              << static_cast<double>(raw.file_bytes) / static_cast<double>(delta.file_bytes)
              << " times smaller than raw\n";
    ::testing::Test::RecordProperty("bytes_per_tick", std::to_string(delta.payload_per_tick));
    return delta.payload_per_tick;
}

constexpr double kTarget = 10.0;

TEST(Compression, AGeneratedMarketTakesNoMoreThanTenBytesATick) {
    // A dozen securities, time moving in microseconds, one side of one quote
    // changing at a time by a cent or by round lots.
    const std::vector<Tick> ticks = test::market_ticks(1, 200'000);
    EXPECT_LE(measured("a generated market", ticks), kTarget);

    // The target is about the whole file too: headers and the index must not
    // be where the bytes went.
    const Size delta = size_of<store::DeltaCodec>(ticks);
    const Size raw = size_of<store::RawCodec>(ticks);
    EXPECT_LE(delta.file_bytes * 3, raw.file_bytes);
}

TEST(Compression, ARealBooksUpdatesTakeNoMoreThanTenBytesATick) {
    // The best bid and offer of books built from the matching engine's feed:
    // prices that are where orders were, sizes that are sums of orders.
    const std::vector<Tick> busy = test::engine_ticks(test::busy_config(1), 200'000);
    ASSERT_GT(busy.size(), 30'000U);
    EXPECT_LE(measured("a busy book", busy), kTarget);

    const std::vector<Tick> thin = test::engine_ticks(test::thin_config(1), 200'000);
    ASSERT_GT(thin.size(), 30'000U);
    EXPECT_LE(measured("a thin book", thin), kTarget);
}

// No target, only the numbers: what the block size costs. Every block starts
// from nothing, so the smaller the blocks, the more ticks have nothing to be
// a difference from. With many securities that is most ticks.
TEST(Compression, WhatSmallBlocksAndManySecuritiesCost) {
    for (const std::uint32_t securities : {12U, 200U, 5'000U}) {
        const std::vector<Tick> ticks = test::market_ticks(1, 200'000, securities);
        std::cout << "[ measured ] " << std::setw(4) << securities << " securities:";
        for (const std::uint32_t per_block : {16U, 256U, 4'096U, 65'536U}) {
            const Size size = size_of<store::DeltaCodec>(ticks, per_block);
            std::cout << "  " << std::fixed << std::setprecision(2) << size.payload_per_tick
                      << " at " << per_block;
            // Whatever the block size, a codec must not write past its own
            // limit, and the limit must be one the writer can live with.
            EXPECT_LE(size.payload_per_tick, static_cast<double>(store::DeltaCodec::kMaxTickSize));
        }
        std::cout << " ticks a block\n";
    }
}

}  // namespace
