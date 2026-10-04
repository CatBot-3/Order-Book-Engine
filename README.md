# Order Book Engine

A C++20 market-data system built on real Nasdaq exchange data. It reads
Nasdaq's TotalView-ITCH 5.0 binary feed and rebuilds the displayed order book
of every listed stock, with a correctness check on every book and a
reproducible latency harness around the whole thing. It is growing into a small
exchange: a price-time-priority matching engine, a lock-free hand-off between
threads, and a TCP order gateway.

The rule of the project: prove the book is right, measure before optimizing,
and log every experiment with a before and an after, including the ones that
did not help.

## Status

| Phase | What | State |
|---|---|---|
| 0 | Build, CI, lint, data fetch | done |
| 1 | ITCH 5.0 parser, `itch_stats`, fuzzing | done; not yet run on a real file |
| 2 | Reference order book | done; not yet run on a real file |
| 3 | Measurement harness | done; no baseline recorded yet |
| 4 | Optimization log | harness, tests and tooling ready; the optimized containers are being written; no experiment logged yet |
| 5 | Matching engine and flow generator | reference engine, flow generator, round trip and tests done; the pooled engine is being written |
| 6 | Lock-free queue and threaded pipeline | not started |
| 7 | TCP gateway and market-data publisher | not started |

No performance number appears in this README until it has been measured on
real data with the method in [`docs/benchmark-method.md`](docs/benchmark-method.md).

## Architecture

```
 file (mmap) ──> [feed]  framing + big-endian decode ──> handler interface (compile-time)
                                  │
                      [book]  order store + price levels, one book per stock locate
                                  │  best bid/offer events
                                  └──> [listeners]  hash for differential testing, stats

 requests ──> [engine]  price-time matching ──> execution reports to the order's owner
                                  └──> market data as ITCH messages ──> back into [feed]

 [bench]  latency histogram, hardware counters, report      [gen]  seeded synthetic order flow

 planned: [util] SPSC ring   [net] epoll gateway, UDP publisher
```

| Directory | Contents |
|---|---|
| `include/obe/feed/` | message structs, codec, `ItchParser<Handler>`, framing |
| `include/obe/book/` | `BookManager`, `Book`, the reference and optimized containers, concepts, the named implementations, hash listener |
| `include/obe/engine/` | the matching-engine contract, `ReferenceEngine`, the hand-written `MatchingEngine`, `ItchFeedWriter`, the round-trip comparison |
| `include/obe/util/` | `LatencyHistogram`, clocks, CPU pinning, perf counters, `MappedFile`, pool and pool allocator, huge-page buffer |
| `include/obe/gen/` | seeded generators: order flow for the engine, and a raw ITCH stream for fixtures |
| `apps/` | `itch_stats`, `book_replay`, `book_view`, `feed_profile`, `flow_gen`, `itch_synth` |
| `bench/` | `replay_bench` (full replay), `engine_bench` (matching engine), `micro_bench` (Google Benchmark) |
| `tests/`, `fuzz/` | unit, scenario, golden, differential, property and round-trip tests; libFuzzer target |
| `scripts/` | data fetch, independent message counter, benchmark runner, book and engine comparison, before/after tables, PGO build |
| `docs/` | [`design.md`](docs/design.md), [`benchmark-method.md`](docs/benchmark-method.md), [`optimization-log.md`](docs/optimization-log.md) |

The library is header-only. Why each piece is shaped the way it is: see
[`docs/design.md`](docs/design.md).

## Build

Linux, or WSL2 on Windows. CMake 3.21+, Ninja, Python 3 for one test, and a
C++20 compiler. Developed and tested with GCC 13 and Clang 18. GoogleTest and
Google Benchmark are fetched by CMake.

```sh
cmake --preset debug                         # Debug with AddressSanitizer and UBSan
cmake --build --preset debug
ctest --preset debug -LE needs-your-code     # everything that is finished
ctest --preset debug -L needs-your-code      # the hand-written parts still being written
```

Other presets: `release` (the only one whose timings mean anything), `tsan`,
`fuzz` (Clang, libFuzzer), and `release-lto` and `release-pgo` for the build
experiments.

```sh
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p fuzz/corpus
build/fuzz/fuzz/parser_fuzz -max_len=4096 -max_total_time=60 fuzz/corpus fuzz/seeds
```

## Data

The data is Nasdaq's public TotalView-ITCH 5.0 sample: one file per trading
day, several gigabytes compressed. It is never committed to this repository.

```sh
scripts/fetch_data.sh -l                                  # list what the server has
scripts/fetch_data.sh -f 01302019.NASDAQ_ITCH50.gz -x     # download, check md5, decompress
```

Read Nasdaq's terms before redistributing any part of it. CI and the tests use
synthetic fixtures made by the project's own generator.

File used for the numbers below: *to be recorded from the first run* (name,
uncompressed size, message count).

## Reproducing the checks

```sh
cmake --preset release && cmake --build --preset release
B=build/release
F=data/01302019.NASDAQ_ITCH50
```

1. **The parser reads the whole day and its counts are right.**
   `itch_stats` reports counts per message type; an independent Python script
   that only walks the length prefixes must agree.

   ```sh
   $B/apps/itch_stats $F
   scripts/check_counts.sh --stats $B/apps/itch_stats --file $F
   ```

2. **The book is consistent for the whole day.** Every invariant counter must
   be zero: no unknown order references, no negative quantities, no empty
   levels left behind.

   ```sh
   $B/apps/book_replay $F
   $B/apps/book_view $F AAPL --at 10:00:00 --depth 10
   ```

3. **An optimized book equals the reference book.** Both replay the whole day
   and must publish the same best-bid-and-offer stream for every security.

   ```sh
   scripts/diff_books.sh --replay $B/apps/book_replay --file $F
   ```

4. **The numbers.** Five runs, median and range, thread pinned, file in
   memory, with the hardware, compiler and flags printed alongside. For any
   implementation other than the reference, the benchmark repeats check 3
   itself and voids its timings if the books differ.

   ```sh
   $B/apps/feed_profile $F                      # the facts the experiments rest on
   scripts/run_bench.sh $F baseline
   scripts/run_bench.sh $F flat-store --impl flat-store
   scripts/compare_runs.py results/<before>/result.json results/<after>/result.json
   ```

5. **The matching engine's feed rebuilds the engine's book.** Seeded order
   flow runs through the engine, which writes its market data as an ITCH file.
   The feed handler reads the bytes back and must arrive at the engine's own
   book; `book_replay` then replays the same file and must find every
   invariant at zero and no locked or crossed update. This check needs no
   Nasdaq data.

   ```sh
   $B/apps/flow_gen --commands 5000000 --symbols 500 --live 50000 data/flow.itch
   scripts/flow_round_trip.sh $B/apps/flow_gen $B/apps/book_replay
   ```

6. **The hand-written engine equals the reference engine.** Same requests,
   same market data byte for byte, same reports to owners.

   ```sh
   scripts/diff_engines.sh --flow-gen $B/apps/flow_gen
   $B/bench/engine_bench --engine reference --cpu 2
   $B/bench/engine_bench --engine pooled --cpu 2
   ```

## Matching rules

These are design choices, stated here so nobody has to infer them from the
code. The full contract is in
[`include/obe/engine/concepts.hpp`](include/obe/engine/concepts.hpp).

1. **Priority is price, then time.** An incoming order trades with the
   best-priced resting order on the other side and, among orders at that
   price, with the one that has waited longest. A trade happens at the resting
   order's price.
2. **A replace that changes the price or increases the size loses time
   priority.** The order goes to the back of its level under a new id. **A
   replace that only decreases the size keeps it**: same id, same place in the
   queue. Letting an order grow in place would let one share reserve a
   position for a larger order later.
3. **Order types:** limit, market, immediate-or-cancel and fill-or-kill, plus
   cancel and replace. A market order never rests. A fill-or-kill order is
   checked against the book before any trade is published, and trades
   completely or not at all.
4. **Self-trading is allowed.** There are no auctions, halts or price bands.

## Results

Filled from real runs only. Method, hardware and flags go with every row.

| Build | Messages/s | p50 | p99 | p99.9 | max | Cache misses per message |
|---|---|---|---|---|---|---|
| Reference (`std::map`, `std::unordered_map`) | | | | | | |
| + flat order table | | | | | | |
| + contiguous price levels | | | | | | |
| + pool allocator | | | | | | |
| Final | | | | | | |

Every experiment, accepted or rejected, is in
[`docs/optimization-log.md`](docs/optimization-log.md).

## Testing

| Layer | What it covers |
|---|---|
| Unit | every decoder against bytes built at specification offsets; containers; histogram |
| Golden | a hand-built byte stream with a hand-worked book |
| Differential | the book against a naive oracle, update for update; each optimized book and the hand-written engine against their reference |
| Property | long seeded random sequences against a `std::map` model; for the engine: never crossed, shares conserved, price-time order, determinism |
| Round trip | the engine's published feed, through the feed handler, rebuilds the engine's book |
| Fuzz | libFuzzer on the parser: no crash, and decode then encode reproduces the input |
| Sanitizers | AddressSanitizer and UBSan on all tests; ThreadSanitizer for the concurrent code to come |
| CI | all of the above on GCC and Clang, plus a formatting check |

## Limitations

1. One venue. This is Nasdaq's own book, not a consolidated view across
   exchanges.
2. Personal hardware. Numbers are from one desktop machine and are labelled
   with it. They are not a claim about production trading systems.
3. Replay from a file. There is no live feed, no gap recovery and no connection
   to a real exchange or broker.
4. Displayed orders only. Hidden liquidity appears in the feed as trade prints
   and is not part of the reconstructed book.
5. Framing and message types are validated; field values are not.
6. The matching engine is exercised with synthetic order flow: random orders
   around a random-walk mid price. It checks the engine and compares one build
   with another. It is not a model of a market.
7. Not a trading strategy, and no claim about profit.
8. Linux only, GCC and Clang only.

This project is not affiliated with or endorsed by Nasdaq.
