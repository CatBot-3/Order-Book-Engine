# Order Book Engine

A C++20 market-data system built on real Nasdaq exchange data. It reads
Nasdaq's TotalView-ITCH 5.0 binary feed and rebuilds the displayed order book
of every listed stock, with a correctness check on every book and a
reproducible latency harness around the whole thing. It is growing into a small
exchange: a price-time-priority matching engine, a lock-free hand-off between
threads, a TCP order gateway and a UDP market-data feed. A market-making
simulator replays the feed and asks what would have happened to quotes placed
in it. The engine can keep a journal of its requests and come back from being
killed holding exactly what it held.

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
| 6 | Lock-free queue and threaded pipeline | mutex queue, seqlock, three-thread pipeline, benchmarks and tests done; the lock-free ring is being written; nothing measured yet |
| 7 | TCP gateway and market-data publisher | done: order gateway, MoldUDP64-style feed with a gap-detecting receiver, open-loop load generator; nothing measured yet |
| 8 | Market-making simulator | fill model, metrics, two simple strategies and `mm_sim` done; the Avellaneda-Stoikov strategy is being written; not yet run on a real file |
| 9 | Journal, snapshot and crash recovery (stretch idea 1) | done: write-ahead journal, torn-tail detection, replay, snapshots, `engine_journal` and a kill test; nothing measured yet |
| 10 | Self-match prevention and more order types (stretch idea 2) | done in the reference engine: post-only, iceberg orders and three self-match modes, through the gateway, the journal and the snapshot; the pooled engine has them to write |
| 11 | Compressed tick store (stretch idea 3) | the file format, queries by time, recovery of an unfinished store, `tick_store` and `tick_bench` done on a codec that does not compress; the compressing codec is being written; nothing recorded from a real file yet |

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

 as three threads:  parser ──queue──> book ──queue──> consumer      [pipeline]
                                     └──> top-of-book board (a seqlock per stock) ──> readers

 [bench]  latency histogram, hardware counters, report      [gen]  seeded synthetic order flow

 as an exchange:    clients ──TCP──> [net] epoll gateway ──> [engine]
                    subscribers <──UDP── [net] MoldUDP64-style packets <── market data

 as a simulation:   file ──> [feed] ──> [sim] fill model + strategy ──> [book]
                                          └──> fills, position, profit, markouts

 surviving a crash: requests ──> [journal] number + checksum ──> file, then ──> [engine]
                    afterwards:  snapshot + journal ──> replay ──> the same engine

 kept for later:    [book] best bid/offer events ──> [store] codec ──> blocks + index on disk
                    a range of time ──> the index ──> the blocks that can hold it ──> ticks
```

| Directory | Contents |
|---|---|
| `include/obe/feed/` | message structs, codec, `ItchParser<Handler>`, framing |
| `include/obe/book/` | `BookManager`, `Book`, the reference and optimized containers, concepts, the named implementations, hash listener |
| `include/obe/engine/` | the matching-engine contract, `ReferenceEngine`, the hand-written `MatchingEngine`, `ItchFeedWriter`, the round-trip comparison |
| `include/obe/pipeline/` | the replay as three threads joined by queues; the top-of-book board |
| `include/obe/net/` | the order-entry protocol, `OrderGateway` on epoll, MoldUDP64-style packetizer and gap-detecting receiver, UDP sockets, the load generator's fixed schedule |
| `include/obe/sim/` | the market-making simulator: fill model, the simple strategies, the hand-written Avellaneda-Stoikov strategy, volatility estimate, report |
| `include/obe/journal/` | the write-ahead journal: records, writer, a reader that tells a torn tail from damage, replay, snapshots, the file layer, recovery from files |
| `include/obe/store/` | the tick store: the codec contract and the reference codec, the hand-written compressing codec, the block file with its index, writer, recorder and reader, the tick profile |
| `include/obe/util/` | `LatencyHistogram`, clocks, CPU pinning, perf counters, `MappedFile`, pool and pool allocator, huge-page buffer; `MutexQueue`, the hand-written `SpscRing`, `SpinChannel`, `Seqlock`; CRC-32, varints |
| `include/obe/gen/` | seeded generators: order flow for the engine, a raw ITCH stream for fixtures, and ticks for the store |
| `apps/` | `itch_stats`, `book_replay`, `book_view`, `feed_profile`, `flow_gen`, `itch_synth`; `exchange_server`, `load_gen`, `md_listen`; `mm_sim`; `engine_journal`; `tick_store` |
| `bench/` | `replay_bench` (full replay), `engine_bench` (matching engine), `queue_bench` (queues, and the ordered stress run), `pipeline_bench` (three threads against one), `journal_bench` (journaling and recovery), `tick_bench` (tick codecs and block sizes), `micro_bench` (Google Benchmark) |
| `tests/`, `fuzz/` | unit, scenario, golden, differential, property, round-trip, recovery and tick-store tests; libFuzzer targets for the parser, the journal and the tick store |
| `scripts/` | data fetch, independent message counter, benchmark runner, book and engine comparison, before/after tables, latency curve, PGO build, the journal crash test, the tick store smoke test |
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
mkdir -p fuzz/journal_corpus
build/fuzz/fuzz/journal_fuzz -max_len=2048 -max_total_time=60 fuzz/journal_corpus fuzz/journal_seeds
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

7. **The lock-free queue is clean.** Two halves, and both are needed. A stress
   run moves one billion numbered items between two threads and checks every
   one. ThreadSanitizer checks the memory ordering, which the stress run
   cannot: x86 orders memory operations more strictly than C++ requires, so a
   ring with the wrong ordering still passes a stress run on it.

   ```sh
   scripts/queue_stress.sh $B/bench/queue_bench ring
   cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
   ```

8. **Does the threaded pipeline beat one thread?** The same file, replayed on
   one thread and as three threads joined by queues, in one run. The two must
   publish the same stream. Whichever is faster, the answer goes in the log.

   ```sh
   $B/bench/queue_bench --queue mutex --cpus 2,4
   $B/bench/queue_bench --queue ring --cpus 2,4
   $B/bench/pipeline_bench $F --queue ring --cpus 2,4,6
   ```

9. **The gateway, measured end to end.** An exchange, a market-data
   subscriber and a load generator, as three processes. The generator sends
   orders over TCP at a fixed rate and times each one from the moment it was
   due to the moment it was acknowledged. The subscriber rebuilds the books
   from the UDP feed and must see no gap.

   ```sh
   scripts/gateway_smoke.sh $B/apps/exchange_server $B/apps/load_gen $B/apps/md_listen

   $B/apps/md_listen --md 127.0.0.1:9002 &
   $B/apps/exchange_server --listen 127.0.0.1:9001 --md 127.0.0.1:9002 &
   $B/apps/load_gen --connect 127.0.0.1:9001 --connections 16 --cpu 4 \
       --rate 1000,5000,20000,50000,100000 --seconds 10 --json results/load.json
   scripts/latency_curve.py results/load.json --plot results/latency_curve.png
   ```

10. **The market-making simulation.** Replay a day and simulate quoting one
    stock. `--from` and `--to` keep the quotes inside continuous trading;
    `--latency-us` is the time between a decision and its effect.

    ```sh
    $B/apps/mm_sim $F --symbol AAPL --strategy fixed --half-spread 1 \
        --from 09:30 --to 16:00 --latency-us 50 --json results/mm_fixed.json
    $B/apps/mm_sim $F --symbol AAPL --strategy join --from 09:30 --to 16:00
    $B/apps/mm_sim $F --symbol AAPL --strategy as --gamma 0.001 --k 200 \
        --from 09:30 --to 16:00
    ```

    The report ends with the assumptions it rests on. Read them before reading
    the profit line.

11. **The engine survives being killed.** The crash test kills a journaling
    engine in the middle of its writes, again and again, and requires the
    journal it ends with to be byte for byte the journal of a run that was
    never killed. `engine_journal` is the program it drives; `journal_bench`
    measures what journaling costs on the disk that `--dir` is on, per batch
    size, with and without `fdatasync`, and how long recovery takes.

    ```sh
    scripts/journal_crash_test.sh $B/apps/engine_journal

    $B/apps/engine_journal run --journal data/obe.journal --snapshot data/obe.snapshot \
        --snapshot-every 50000 --commands 1000000 --batch 64 --sync flush
    $B/apps/engine_journal check --journal data/obe.journal --snapshot data/obe.snapshot
    $B/apps/engine_journal dump --journal data/obe.journal --limit 20

    $B/bench/journal_bench --dir data --cpu 4 --json results/journal.json
    ```

    Interrupt the `run` with `kill -9` and start it again with the same
    options: it says what it recovered from and carries on.

12. **A day's quotes on disk, asked by time.** `record` replays a file through
    the book and stores every best-bid-and-offer update; `verify --against`
    replays it again and checks the store holds exactly what the book
    publishes; `query` reads only the blocks a range of time can touch.
    `profile` prints what consecutive ticks differ by, which is the table to
    design the compressing codec from. Until that codec is written, `--codec
    delta` exits with status 4 and a store is 34 bytes a tick.

    ```sh
    scripts/tick_store_smoke.sh $B/apps/itch_synth $B/apps/tick_store

    $B/apps/tick_store profile $F
    $B/apps/tick_store record --codec raw $F data/day.ticks
    $B/apps/tick_store verify --against $F data/day.ticks
    $B/apps/tick_store info data/day.ticks
    $B/apps/tick_store query --symbol AAPL --from 10:00:00 --to 10:00:01 data/day.ticks

    $B/bench/tick_bench --file $F --cpu 4 --json results/ticks.json
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
4. **Post-only** orders rest or do nothing: one that would trade on arrival is
   cancelled, and a replace that would make one trade is refused.
5. **Iceberg orders** show a part and hide the rest. The market data and the
   level totals carry displayed shares only. When the displayed part is used
   up the next slice appears at the back of the queue under a new order
   reference, so nobody watching can tell it from a new order.
6. **Self-match prevention is per order.** By default an owner's order can
   trade with another of their own. An order can ask instead for the incoming
   order to be cancelled, the resting one, or both, at the moment the two
   would trade.
7. There are no auctions, halts, price bands, stop orders or pegged orders.

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

### Order-to-acknowledgement latency

Loopback only: the client and the server are on one machine and no packet
reaches a network card. These numbers measure the software path (system calls,
the kernel's TCP stack, the gateway, the engine), not a network. Latency is
counted from each request's scheduled send time, so a stalled server cannot
hide its own delay.

| Requests/s | p50 µs | p99 µs | p99.9 µs | max µs | Generator lag max µs |
|---|---|---|---|---|---|
| | | | | | |

Filled by `scripts/latency_curve.py` from a real run, with the machine, the
number of connections and the CPUs each program was pinned to.

## Testing

| Layer | What it covers |
|---|---|
| Unit | every decoder against bytes built at specification offsets; containers; histogram |
| Golden | a hand-built byte stream with a hand-worked book |
| Differential | the book against a naive oracle, update for update; each optimized book and the hand-written engine against their reference |
| Property | long seeded random sequences against a `std::map` model; for the engine: never crossed, shares conserved, price-time order, determinism, on flows with and without icebergs, post-only orders and self-match prevention |
| Round trip | the engine's published feed, through the feed handler, rebuilds the engine's book |
| Concurrency | each queue: every item once, in order, intact, between two threads; the pipeline against the single-threaded replay; the seqlock against torn reads |
| Network | the gateway over real loopback sockets: partial reads, mid-message disconnects, bad input, slow and paused clients; the feed with packets lost, repeated and reordered; the load generator's schedule against a server that stalls, in simulated time |
| Simulation | each rule of the fill model on hand-built messages; over generated markets, the queue ahead of every quote against the test's own copy of the orders, and the profit recounted from the fills |
| Recovery | an engine rebuilt from its journal, or from a snapshot and the journal after it, holds, said and goes on to say exactly what the original did; a journal cut anywhere recovers to the last whole record; the real program, killed repeatedly with `SIGKILL` in the middle of writes, ends with the journal of a run that was never killed |
| Tick store | each codec gives back every tick, likely or not, and reads arbitrary bytes without leaving them; ranges of time against a filter over the plain list; a store that was never finished, cut at every byte, or changed one bit at a time never yields a tick that was not written; an index that checks out and lies is not believed; the real program against a fresh replay of the feed |
| Fuzz | libFuzzer on the parser: no crash, and decode then encode reproduces the input. On the journal: any cut gives a prefix of the requests, and any changed byte is noticed and never applied. On the tick store: a file that opens describes itself consistently, and a store with one byte changed never yields a tick that was not written |
| Sanitizers | AddressSanitizer and UBSan on all tests; ThreadSanitizer on all of them too, which is what checks the queues' memory ordering |
| CI | all of the above on GCC and Clang, plus a formatting check |

## Limitations

1. One venue. This is Nasdaq's own book, not a consolidated view across
   exchanges.
2. Personal hardware. Numbers are from one desktop machine and are labelled
   with it. They are not a claim about production trading systems.
3. No real exchange. Market data comes from a file or from this project's own
   exchange. There is no live feed, no connection to a broker, and a gap in the
   UDP feed is detected but not recovered.
4. Loopback networking. The gateway's numbers measure the software path on one
   machine, not a network, and the order-entry protocol has no login or
   authentication: it is for a lab, not for anything exposed.
5. Displayed orders only. Hidden liquidity appears in the feed as trade prints
   and is not part of the reconstructed book.
6. Framing and message types are validated; field values are not.
7. The matching engine is exercised with synthetic order flow: random orders
   around a random-walk mid price. It checks the engine and compares one build
   with another. It is not a model of a market.
8. Not a trading strategy, and no claim about profit. The market-making
   simulator is a simulation with stated assumptions: the recorded market
   never saw its quotes and did not react to them. It compares strategies
   under those assumptions and shows adverse selection; it does not estimate
   what a strategy would earn.
9. The journal is not connected to the exchange. `engine_journal` shows an
   engine recovering; `exchange_server` runs without a journal, because doing
   it properly means holding every response until its request is on disk and
   giving clients a way to learn what survived. One file on one disk: no
   rotation and no second copy.
10. The tick store keeps the best bid and offer and nothing else: no depth,
    no trades. It is one stream in time order, so a query for one security
    reads every block in its range of time. No size or speed is quoted for
    it: nothing has been recorded from a real day.
11. Linux only, GCC and Clang only.

This project is not affiliated with or endorsed by Nasdaq.
