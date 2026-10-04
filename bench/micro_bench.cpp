// Micro benchmarks of the building blocks, with Google Benchmark.
//
//   build/release/bench/micro_bench
//   build/release/bench/micro_bench --benchmark_filter=Parse --benchmark_repetitions=5
//
// These isolate one small operation each. They answer "what does a decode
// cost?" and are useful when an experiment changes one of them. They do not
// replace replay_bench: an operation measured alone, in a tight loop with warm
// caches and a trained branch predictor, costs less than the same operation
// inside a real replay.

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "obe/feed/codec.hpp"
#include "obe/feed/endian.hpp"
#include "obe/feed/parser.hpp"
#include "obe/gen/synthetic_feed.hpp"
#include "obe/util/clock.hpp"
#include "obe/util/latency_histogram.hpp"

namespace {

using namespace obe;

// --- Loads ----------------------------------------------------------------------

void BM_LoadBe32(benchmark::State& state) {
    std::array<std::byte, 16> buf{};
    feed::store_be<std::uint32_t>(buf.data() + 1, 0x01020304U);
    const std::byte* p = buf.data() + 1;  // misaligned on purpose
    for (auto _ : state) {
        benchmark::DoNotOptimize(p);
        benchmark::DoNotOptimize(feed::load_be<std::uint32_t>(p));
    }
}
BENCHMARK(BM_LoadBe32);

void BM_LoadBe48(benchmark::State& state) {
    std::array<std::byte, 16> buf{};
    feed::store_be48(buf.data() + 5, 34'200'000'000'000ULL);
    const std::byte* p = buf.data() + 5;
    for (auto _ : state) {
        benchmark::DoNotOptimize(p);
        benchmark::DoNotOptimize(feed::load_be48(p));
    }
}
BENCHMARK(BM_LoadBe48);

// --- Decoding one message -------------------------------------------------------------

void BM_DecodeAddOrder(benchmark::State& state) {
    feed::AddOrder msg;
    msg.hdr = {.locate = 42, .tracking = 0, .timestamp = 34'200'000'000'000ULL};
    msg.order_ref = 123'456'789;
    msg.side = Side::Buy;
    msg.shares = 300;
    msg.stock = feed::Symbol::from("AAPL");
    msg.price = 1'728'036;
    std::array<std::byte, feed::kMaxMessageSize> buf{};
    feed::encode(msg, buf.data());
    const std::byte* p = buf.data();
    for (auto _ : state) {
        benchmark::DoNotOptimize(p);
        benchmark::DoNotOptimize(feed::decode<feed::AddOrder>(p));
    }
}
BENCHMARK(BM_DecodeAddOrder);

// --- Parsing a stream -----------------------------------------------------------------

struct SumHandler : feed::HandlerBase {
    std::uint64_t sum = 0;
    void on_add(const feed::AddOrder& m) noexcept { sum += m.order_ref + m.price + m.shares; }
    void on_execute(const feed::OrderExecuted& m) noexcept { sum += m.order_ref + m.shares; }
    void on_execute_with_price(const feed::OrderExecutedWithPrice& m) noexcept {
        sum += m.order_ref + m.shares + m.price;
    }
    void on_cancel(const feed::OrderCancel& m) noexcept { sum += m.order_ref + m.shares; }
    void on_delete(const feed::OrderDelete& m) noexcept { sum += m.order_ref; }
    void on_replace(const feed::OrderReplace& m) noexcept {
        sum += m.orig_order_ref + m.new_order_ref + m.price + m.shares;
    }
};

// Synthetic input: the message mix is not the real feed's. Compare this number
// with itself across changes, not with anything measured on real data.
void BM_ParseSyntheticStream(benchmark::State& state) {
    const std::vector<std::byte> stream = gen::make_synthetic_feed(
        {.seed = 1, .symbols = 64, .messages = static_cast<std::uint64_t>(state.range(0))});
    std::uint64_t messages = 0;
    for (auto _ : state) {
        SumHandler handler;
        feed::ItchParser parser(handler);
        const feed::ParseResult result = parser.parse(stream);
        benchmark::DoNotOptimize(handler.sum);
        messages += result.messages;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(messages));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(stream.size()));
}
// 10'000 messages fit in the L2 cache; 1'000'000 do not.
BENCHMARK(BM_ParseSyntheticStream)->Arg(10'000)->Arg(1'000'000);

// --- The measurement tools themselves ---------------------------------------------------

void BM_HistogramRecord(benchmark::State& state) {
    util::LatencyHistogram hist;
    gen::SplitMix64 rng(1);
    std::array<std::uint64_t, 1024> values{};
    for (std::uint64_t& v : values) {
        v = 20 + rng.below(400);  // tens to hundreds, like per-message tick counts
    }
    // Let the histogram's address escape and force its stores to happen on
    // every iteration. Without this the compiler folds the whole loop into a
    // handful of additions and the benchmark reports 0 ns: the "compiler
    // deleted the work" mistake the benchmark method warns about.
    benchmark::DoNotOptimize(&hist);
    std::size_t i = 0;
    for (auto _ : state) {
        hist.record(values[i]);
        benchmark::ClobberMemory();
        i = (i + 1) & (values.size() - 1);
    }
}
BENCHMARK(BM_HistogramRecord);

void BM_ClockTsc(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(util::TscClock::now());
    }
}
BENCHMARK(BM_ClockTsc);

void BM_ClockSteady(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(util::SteadyClock::now());
    }
}
BENCHMARK(BM_ClockSteady);

}  // namespace
