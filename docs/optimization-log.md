# Optimization log

One experiment at a time, each with a number before and a number after. Failed
experiments stay in the log with their numbers: knowing what did not help, and
why, is as much a result as a speed-up.

Rules (from the project spec, phase 4):

1. Measure with the method in [`benchmark-method.md`](benchmark-method.md).
   Same file, same machine, same flags for "before" and "after".
2. One change per entry.
3. After every accepted change the optimized book's best-bid-and-offer hash
   must equal the reference book's on the same file. If it does not, the entry
   is void whatever the timing says. `replay_bench` checks this itself and
   says so in its report.
4. Write the entry yourself, in your own words, from your own runs.

## How to run an experiment

```sh
B=build/release
F=data/<file>

# 0. Once: the facts about the feed that the hypotheses rest on.
$B/apps/feed_profile $F

# 1. Before. Keep the JSON.
scripts/run_bench.sh $F baseline --reserve <peak resting orders>

# 2. Write the hypothesis in this file. Then write the code.

# 3. Correct first: the tests, then the full day against the reference.
ctest --preset debug -L core
scripts/diff_books.sh --replay $B/apps/book_replay --file $F --impl flat-store

# 4. After.
scripts/run_bench.sh $F flat-store --impl flat-store --reserve <peak resting orders>

# 5. The table for the entry.
scripts/compare_runs.py results/<before>/result.json results/<after>/result.json
```

`compare_runs.py` prints the before/after table and lists anything that makes
the comparison unfair: a different file, machine or flags, fewer than five
runs, a book that did not match the reference, or a change smaller than the
run-to-run range.

## Environment

The first real-data run, 2026-10-16. It ran in the Claude session's cloud
machine because the WSL2 install on the owner's PC needs a restart; the day
was downloaded on the PC and copied across. Run again on the PC later and add
its columns: these numbers belong to this machine only.

| | |
|---|---|
| Machine | Cloud virtual machine of a Claude session (shared hardware) |
| CPU | Intel Xeon Processor @ 2.10GHz as reported, 2 logical CPUs, under a hypervisor |
| Memory | 7.8 GiB |
| OS / kernel | Ubuntu 24.04.5 LTS, Linux 6.18.44-fc-v139 x86_64 |
| Native Linux, WSL2 or VM | VM |
| Compiler | GCC 13.3.0 |
| Flags | Release: `-O3 -DNDEBUG` (the LTO row adds `-flto`) |
| Data file | `01302019.NASDAQ_ITCH50` (Nasdaq TotalView-ITCH 5.0, 30 January 2019); `.gz` 4,764,426,091 bytes, md5 `54b7afd0be7f925f7e9d67a23898827b` |
| Uncompressed size | 11,245,883,092 bytes |
| Messages | 368,366,634 (8,714 symbols; 03:03:59.69 to 20:05:00.00) |
| Pinned CPU, governor | CPU 1 of 0 and 1; governor not exposed |
| Hardware counters available | No (`perf_event_open` is not available in this VM), so no cache-miss figures |
| Transparent huge pages setting | `madvise` |

Two departures from [`benchmark-method.md`](benchmark-method.md), both forced
by the machine, and both to keep in mind when reading any number below:

1. The file is larger than the memory, so `replay_bench` ran with
   `--no-copy`: every pass reads the file through the page cache, and part of
   it from the virtual disk (`vmstat` showed 55 to 75 MB/s of reads during
   the reference row; nothing in the results records it).
   The `max` column is therefore mostly page faults and table growth, not
   the code. Throughput comparisons are fair, because every row paid the
   same, but they understate differences slightly.
2. Two shared virtual CPUs. A single-threaded row is pinned and its run-to-run
   spread is reported; anything with three threads (the pipeline, the
   gateway with its subscriber and load generator) has fewer CPUs than
   threads here, and its numbers say little.

## Feed profile

From `feed_profile` on the data file (`results/real-cloud/profile/feed_profile.txt`).

| | |
|---|---|
| Most orders resting at once | 1,742,866, at 15:57:46.28 |
| Reference-number density (orders added / span) | 58.30% (191,919,099 added over a span of 329,176,550) |
| New reference above all earlier ones | 24.45% |
| Levels on a side when updated: median / 99th / largest | 55 / 1,791 / 3,143 (AMZN bids) |
| Updates at the best price | 45.25% |
| Updates within 5 levels of the best | 74.32% |
| Updates 64 or more levels deep | 1.16% |
| Share of order messages in the busiest 10 securities | 5.46% |

## Baseline

The reference build: `std::unordered_map` order store, `std::map` price levels.
Five measured runs after one warm-up, pinned to CPU 1, `--no-copy` (see
Environment). Latencies in ns per message, medians over the runs.

| Workload | msgs/s (median) | range | mean ns | p50 | p99 | p99.9 | max | cache misses / msg | timer cost p50 |
|---|---|---|---|---|---|---|---|---|---|
| `parse` | 35,535,049 | 31,617,257 to 40,544,249 | 28.1 | 34.8 | 59.5 | 167.1 | 55.7 ms | n/a | 19.1 |
| `book` (`--impl reference`) | 2,361,505 | 2,290,975 to 2,385,792 | 423.5 | 426.2 | 1,645.2 | 14,140.5 | 177.5 ms | n/a | 18.1 |

Reference best-bid-and-offer hash for this file: `73b11ec0681d1994`

Two consecutive invocations agreed within: not measured in this run; within
one invocation the five runs of the reference book spread 4.0% (the parse
row 25%, which reads the disk hardest).

## Entry template

```
### N. <short name>

Status: accepted | rejected | inconclusive
Date:
Commit before / after:

Hypothesis
  What I expect to change, by roughly how much, and the mechanism. Written
  before measuring. Cite the feed profile where the hypothesis rests on it.

Change
  What was changed, in one or two sentences.

Method
  Anything beyond the standard method. Otherwise "standard".

Before / After
  The table from scripts/compare_runs.py.

Correctness
  Hash equals the reference: yes | no.

Verdict
  Accepted or rejected, and the size of the effect.

Why
  What actually happened and how I know. If the hypothesis was wrong, what the
  numbers showed instead.
```

## Backlog

Candidates from the project spec, roughly in order of expected payoff. "Ready"
means the harness can already run the experiment; the third column is the
code the experiment is about (all of it written now) or what is still to
decide. `scripts/real_data_evals.sh` runs the ready ones on a real file.

| # | Experiment | What is ready | The code, or what is left | Status |
|---|---|---|---|---|
| 1 | Order store: `std::unordered_map` to a pre-sized open-addressing flat table | `--impl flat-store`, `--reserve`, contract tests, differential check | `include/obe/book/flat_order_store.hpp` | accepted, entry 1 |
| 2 | Order store: direct index by reference number | `feed_profile` reports the density and ordering of reference numbers | Decide from the profile whether it is viable. If the span is far larger than the peak of resting orders, log it as rejected with those two numbers and do not build it | rejected from the profile, entry 2 |
| 3 | Price levels: `std::map` to a sorted contiguous vector, best price at the back | `--impl vector-levels`, contract tests, differential check; `feed_profile` reports where updates land | `include/obe/book/vector_price_levels.hpp` | accepted, entry 3 |
| 4 | Pool allocator for orders | `--impl pooled` (the reference containers with pooled nodes), the allocator adapter, pool tests | `include/obe/util/pool.hpp` | inconclusive, entry 4 |
| 5 | Struct layout: shrink the order record, split hot and cold fields | `feed_profile` prints the record sizes | The layout inside `FlatOrderStore` (keys and records together or apart) | not run |
| 6 | Software prefetch of the next message's order slot | `replay_bench --prefetch` reads one message ahead and calls `prefetch(id)` on the store | A `prefetch` member on `FlatOrderStore` | accepted, entry 6 |
| 7 | Dispatch: `switch` against a jump table against a length table | Nothing yet. The parser has one dispatcher | Ask for the alternative dispatchers when you reach this | not run |
| 8 | Huge pages for the order store | `util::HugePageBuffer`; the report prints the system setting and the huge-page bytes in use | Back the flat table's slot array with it | not run |
| 9 | Symbol filter: process only a watch list | `replay_bench --symbols A,B,C` | Choose watch lists of different sizes and plot cost against the share of messages applied | not run |
| 10 | Link-time and profile-guided optimization | `release-lto` preset; `scripts/pgo_build.sh` | Run them. Expect little from LTO: every app is a single translation unit. Say what the training data for PGO was | LTO inconclusive, PGO +15% on its training day: entries 7 and 8 |
| 11 | Matching engine: node-based reference to pooled orders in intrusive queues | `engine_bench --engine reference` and `--engine pooled` on the same recorded tape; the differential test and `scripts/diff_engines.sh` | `include/obe/engine/matching_engine.hpp`. Count the allocations each engine makes per resting order and say which of them the pool removed | accepted, entry 9 |
| 12 | Queue: mutex and condition variable, to the same lock with spinning, to a lock-free ring | `queue_bench --queue mutex`, `mutex-spin`, `ring`; contract tests; `scripts/queue_stress.sh` | `include/obe/util/spsc_ring.hpp`. Log the two steps separately (what sleeping costs, what the lock costs), then the ring with and without the cached indices. State the CPUs the two threads were pinned to | measured, entry 10 |
| 13 | Three-thread pipeline against the single-threaded replay | `pipeline_bench <file> --queue NAME --cpus P,B,C`, which measures both and checks they agree | Run it on the real file. Record the ratio either way, with the wait counts that explain it | needs three free cores, entry 11 |
| 14 | Tick store: every field at full width, to differences in variable-length integers | `tick_store profile` (what consecutive ticks differ by), `tick_store record --codec raw` and `--codec delta`, `tick_bench`, the codec contract tests and a first size target | `include/obe/store/delta_codec.hpp`. Put the profile of the real file in the entry before the design. Log bytes per tick and ticks per second for each step, the block size they were taken at, and the raw store through `zstd -19` beside them | accepted, entry 13 |
| 15 | Order gateway: readiness (epoll) to completion (io_uring) | `transport_bench --cpus S,C` (system calls per request and requests per second, by number of busy connections), `exchange_server --io NAME` with `load_gen` for the latency curve, the transport contract tests on either | `include/obe/net/uring_transport.hpp`. Log the first correct version as it is, with system calls per request beside the latency curve for both transports, the kernel version and the mitigations in force. Then one entry per step listed at the end of that header | measured, entry 12 |

`--impl flat-vector` combines experiments 1 and 3 for the cumulative row in the
README's results table.

## Entries

These entries were written by Claude from the run of 2026-10-16
(`results/real-cloud/`, gitignored; the summary is `SUMMARY.md` there), at
the owner's request. Rule 4 above asks for entries in the owner's own words
from the owner's own runs: rewrite them when the run is repeated on the PC.
Every row is on the machine in "Environment", with the departures listed
there. Before/after tables are `scripts/compare_runs.py` output, abridged to
throughput, p50 and p99; the full tables are in `results/real-cloud/bench/`,
`lto/` and `pgo/`. The code measured is commit `1eb072c` (`718bda9` in the
session's copy of the repository, the same tree); the uncommitted changes at
the time of the run were to the scripts only.

### 1. Order store: a pre-sizable open-addressing table

Status: accepted
Date: 2026-10-16
Commit before / after: `1eb072c` for both (`--impl reference` against `--impl flat-store`)

Hypothesis
  `std::unordered_map` allocates a node per order and chases two pointers per
  lookup (bucket, then node). With 1,742,866 orders resting at the peak the
  table is far larger than any cache, so most lookups miss at least twice.
  A flat table with linear probing at 70% load misses about once. Expect
  10% to 30% more throughput: the order store is one of two structures every
  message touches, and the price levels are the other.

Change
  `FlatOrderStore` (Fibonacci hash, linear probing, backward-shift deletion,
  24-byte slots holding key, record and flag together).

Method
  Standard, with the departures in "Environment".

Before / After
  | | Before | After | Change |
  |---|---|---|---|
  | msgs/s | 2,361,505 (2,290,975 to 2,385,792) | 2,668,257 (2,613,460 to 2,764,043) | +13.0% |
  | p50 ns | 426.2 | 342.4 | -19.7% |
  | p99 ns | 1,645.2 | 1,492.9 | -9.3% (within the run-to-run range) |

Correctness
  Hash equals the reference: yes (`73b11ec0681d1994`), and
  `scripts/diff_books.sh` matched all 8,695 securities.

Verdict
  Accepted: 1.13x throughput, a fifth off the median message.

Why
  The median message got 84 ns cheaper, about the cost of one memory access. The gain is
  smaller than the price levels' (entry 3) because on this day the levels are
  the bigger cost: a side has 55 levels at the median and 1,791 at the 99th
  percentile, and a `std::map` walk over that many nodes is several misses.
  Without counters this rests on the timing alone.

### 2. Order store: an array indexed by reference number

Status: rejected (not built)
Date: 2026-10-16

Hypothesis
  If reference numbers were dense and increasing, an array indexed by them
  would make every lookup one load with no hashing.

Change
  None: the feed profile decided it.

Before / After
  The profile: reference numbers span 329,176,550 values for a peak of
  1,742,866 resting orders, a density of 58.30% over the day but 0.53% at the
  peak; only 24.45% of new references are above all earlier ones. An array
  would need 329 million slots, 189 times the peak: about 4 GB of 12-byte
  records (2.6 GB as 8-byte pointers) against the flat table's 96 MiB.

Verdict
  Rejected: about 40 times the memory of the flat table (26 times as
  pointers), and the slots in use at any moment are scattered across it, so
  the cache would gain nothing.

Why
  The numbers are unique for the day across all securities, and they are not
  handed out in one increasing sequence (only a quarter of new ones exceed
  every earlier one), so the live ones are a sparse, shifting subset of a
  large range.

### 3. Price levels: a sorted vector with the best price at the back

Status: accepted
Date: 2026-10-16
Commit before / after: `1eb072c` for both (`--impl reference` against `--impl vector-levels`)

Hypothesis
  45.25% of level updates are at the best price and 74.32% within five levels
  of it; only 1.16% are 64 or more levels deep. A sorted array with the best
  at the back finds the common case in a short scan of one or two cache lines,
  where `std::map` walks a tree of separately allocated nodes. Inserting or
  removing deep in a long side costs a memmove, but the profile says that is
  rare. Expect a larger gain than entry 1.

Change
  `VectorPriceLevels` (16-byte entries, the side folded into the key, a scan of
  the last eight entries then binary search).

Method
  Standard, with the departures in "Environment".

Before / After
  | | Before | After | Change |
  |---|---|---|---|
  | msgs/s | 2,361,505 (2,290,975 to 2,385,792) | 3,399,704 (3,239,731 to 3,513,109) | +44.0% |
  | p50 ns | 426.2 | 281.4 | -34.0% |
  | p99 ns | 1,645.2 | 1,127.1 | -31.5% |

Correctness
  Hash equals the reference: yes, and `diff_books.sh` matched all 8,695
  securities.

Verdict
  Accepted: 1.44x throughput; the largest single gain of the day.

Why
  The tail improved as much as the median, which says the deep updates did
  not turn into a new tail: the memmove of a few hundred 16-byte entries
  costs less than the misses of a tree walk. The busiest side of the day
  (AMZN bids, 3,143 levels) is the case to profile if a tail appears on
  another day.

### 4. A pool allocator for the reference containers' nodes

Status: inconclusive
Date: 2026-10-16
Commit before / after: `1eb072c` for both (`--impl reference` against `--impl pooled`)

Hypothesis
  The reference containers allocate a node for every order and every new
  level. Taking them from `util::Pool` removes the general allocator from the
  path and reuses the slot that was just freed while it is still in cache.
  Expect a small gain: the allocator is fast, and the lookups' misses remain.

Change
  `--impl pooled`: the same containers with `PoolAllocator`.

Before / After
  | | Before | After | Change |
  |---|---|---|---|
  | msgs/s | 2,361,505 (2,290,975 to 2,385,792) | 2,473,392 (2,421,235 to 2,564,187) | +4.7% |
  | p50 ns | 426.2 | 426.2 | 0% |

Correctness
  Hash equals the reference: yes.

Verdict
  Inconclusive: the change is smaller than the run-to-run range by
  `compare_runs.py`'s rule, though the two ranges do not overlap.

Why
  The median message did not move: allocation was not where the time went.
  The flat table (entry 1) removes the nodes altogether, which is the better
  fix for the order store; the pool still serves the matching engine
  (entry 9).

### 5. Pre-sizing the order store

Status: rejected for the reference store, inconclusive for the flat table
Date: 2026-10-16

Hypothesis
  `--reserve 1917152` (the peak plus a tenth) sizes the table once, so it
  never grows during the day. Expect the max latency to drop (no rehash of a
  table with a million entries in the middle of a message) and throughput to
  change little.

Before / After
  | | Before | After | Change |
  |---|---|---|---|
  | reference, msgs/s | 2,361,505 (2,290,975 to 2,385,792) | 2,002,415 (1,959,665 to 2,048,702) | -15.2% |
  | reference, max | 177.5 ms | 40.2 ms | -77.3% |
  | flat table, msgs/s | 4,712,324 (4,622,151 to 4,768,703) | 4,780,096 (4,654,616 to 4,823,378) | +1.4% (noise) |
  | flat table, max | 81.4 ms | 38.5 ms | -52.7% |

Correctness
  Hash equals the reference: yes, both.

Verdict
  The max fell as expected in both. For `std::unordered_map` pre-sizing cost
  15% of throughput, so it is rejected there; for the flat table the
  throughput change is noise, and it is kept for the max.

Why
  A rehash is a stall proportional to the table: that is the drop in max.
  The throughput loss of the pre-sized `std::unordered_map` is a guess without
  counters: its bucket array is sized for the peak from the first message, so
  for most of the day each lookup's first load lands in a 16 MB array of
  mostly empty buckets instead of one that grew with the book and stayed
  denser in cache.

### 6. Software prefetch of the next message's order

Status: accepted
Date: 2026-10-16
Commit before / after: `1eb072c` for both (`--impl flat-vector --reserve 1917152`, without and with `--prefetch`)

Hypothesis
  With 1.7 million resting orders the flat table is 96 MiB, so nearly every
  lookup misses to memory. Reading the next message's reference number and
  prefetching its slot before handling the current message overlaps that
  miss with useful work. Expect up to the cost of one miss per message.

Change
  `replay_bench --prefetch`, which calls `FlatOrderStore::prefetch` one message
  ahead.

Before / After
  | | Before | After | Change |
  |---|---|---|---|
  | msgs/s | 4,780,096 (4,654,616 to 4,823,378) | 6,104,579 (5,787,386 to 6,258,674) | +27.7% |
  | p50 ns | 224.3 | 163.3 | -27.2% |
  | p99 ns | 731.0 | 609.0 | -16.7% |

Correctness
  Hash equals the reference: yes.

Verdict
  Accepted: 1.28x, and the fastest book of the day, 2.59x the reference.

Why
  The median fell by 61 ns, close to one memory access, which is what a
  prefetch can hide when it is issued one message ahead. That it worked this
  well also says the lookup was the main miss left once the levels were
  contiguous.

### 7. Link-time optimization

Status: inconclusive
Date: 2026-10-16

Hypothesis
  Every program is a single translation unit, so the compiler already sees
  all of it; expect nothing measurable.

Before / After
  `--impl flat-vector --reserve 1917152`, release against `release-lto`:
  4,780,096 to 4,912,284 msgs/s (+2.8%), p50 224.3 to 228.1 ns, p99 unchanged.

Verdict
  Inconclusive, as expected: within the run-to-run range.

### 8. Profile-guided optimization

Status: accepted, with the caveat below
Date: 2026-10-16

Hypothesis
  Unlike LTO, PGO tells the compiler which branches go which way and which
  paths are hot: the parser's dispatch on the message type, the book's
  common cases (an update at the best level, an order found on the first
  probe). Expect a few percent, mostly from code layout.

Method
  `scripts/pgo_build.sh`: an instrumented build, one training replay of the
  whole day, a rebuild with the profile. Trained on the same day it is then
  measured on, which is the best case for PGO; a fair result trains on one
  day and measures another. (The first attempt failed: the training run
  asked for an in-memory copy of a file larger than the memory. The script
  now passes `--no-copy` there too.)

Before / After
  `--impl flat-vector --reserve 1917152`:
  | | Before | After | Change |
  |---|---|---|---|
  | msgs/s | 4,780,096 (4,654,616 to 4,823,378) | 5,494,735 (5,314,371 to 5,546,984) | +15.0% |
  | mean ns | 209.2 | 182.0 | -13.0% |
  | p50 ns | 224.3 | 224.3 | 0% |
  | p99 ns | 731.0 | 731.0 | 0% (within the run-to-run range) |

Correctness
  Hash equals the reference: yes.

Verdict
  Accepted as a measured best case: +15% throughput, far outside the
  run-to-run range. Not yet shown to hold on a day the profile was not
  trained on.

Why
  The mean fell and the percentiles did not, so the gain is spread thinly
  over all messages rather than taken off the slow ones: the signature of
  better code layout and branch arrangement, which cost a few nanoseconds on
  every message. (`p50` and `p99` move in steps of the histogram's buckets,
  so a few nanoseconds can stay inside one.) This run did not combine PGO
  with the prefetch of entry 6; the two together are the next experiment.

### 9. Matching engine: pooled orders in intrusive queues

Status: accepted
Date: 2026-10-16

Hypothesis
  The reference engine allocates, per resting order, a `std::list` node and an
  index node, plus a tree node when its price is new. `MatchingEngine` takes
  the order from a pool, links it into its level without allocating, and finds
  it through an array of chunks indexed by id. Expect a large gain on the
  allocations and the index lookup.

Before / After
  `engine_bench`, 2,000,000 recorded requests of synthetic flow, five runs:
  | | reference | pooled | |
  |---|---|---|---|
  | requests/s | 6,062,610 (4,332,542 to 6,524,720) | 12,581,950 (9,657,990 to 14,737,579) | 2.08x |
  | p50 ns | 182.4 | 102.4 | |
  | with post-only, icebergs and self-match prevention | 6,082,820 | 14,033,889 | 2.31x |

Correctness
  Market data and reports identical to the reference engine's, request by
  request (`engine_bench` checks; `scripts/diff_engines.sh` too).

Verdict
  Accepted: about twice the throughput. The spreads are wide (up to 40%) on
  this machine, but the two ranges are far apart.

### 10. The queue between two threads

Status: measured; see the caveat
Date: 2026-10-16

`queue_bench`, producer on CPU 1, consumer on CPU 0, 40-byte items:
`std::mutex` with condition variables 4,412,509 items/s, the same lock with
spinning 4,090,578, the lock-free ring 74,891,081 (17x the mutex). The ordered
stress run moved 1,000,000,000 items through the ring, every one once, in
order and intact, and ThreadSanitizer was silent on the queue and pipeline
tests. The two locks are within each other's run-to-run range (spreads of
10.4% and 21.8%), so this run does not say whether spinning or sleeping is
cheaper; measure again on the PC, where the spread should be smaller.

### 11. The three-thread pipeline against one thread

Status: not meaningful on this machine
Date: 2026-10-16

`pipeline_bench` on the day: with the ring the pipeline ran at 0.97 times the
single thread (3,683,280 against 3,789,898 msgs/s), with the mutex at 0.31.
Output identical to the single thread's either way. With two CPUs the parser
and the consumer shared one, so the pipeline could not win; this needs three
free cores.

### 12. The order gateway: epoll against io_uring

Status: measured; latency curve not meaningful on this machine
Date: 2026-10-16

`transport_bench` (kernel 6.18.44): system calls per request fall with the
number of busy connections for both, from 3.00 to 2.12 for epoll and from 2.00
to 0.04 for io_uring (1 to 256 connections); requests per second were about
equal at 256 connections (187,414 and 192,346). The latency curve
(`results/real-cloud/gateway/latency_curve.md`) ran the exchange, the
subscriber and the load generator on two CPUs: the generator itself fell up to
16 ms behind its schedule, so the tails describe the machine, not the gateway.
Every request was acknowledged at every rate.

  p50 / p99 / p99.9 in µs, 16 connections, 10 s per rate after 1 s of warm-up:
  | Requests/s | epoll, reference engine | epoll, pooled engine | io_uring, pooled engine |
  |---|---|---|---|
  | 1,000 | 63.5 / 901.1 / 3,407.9 | 69.6 / 1,441.8 / 3,735.6 | 67.6 / 1,245.2 / 3,604.5 |
  | 5,000 | 24.6 / 483.3 / 2,490.4 | 24.6 / 819.2 / 3,342.3 | 27.6 / 491.5 / 2,949.1 |
  | 20,000 | 22.5 / 540.7 / 4,980.7 | 24.6 / 1,146.9 / 25,165.8 | 25.1 / 2,359.3 / 30,933.0 |
  | 50,000 | 18.9 / 1,638.4 / 10,747.9 | 19.5 / 1,671.2 / 5,111.8 | 39.9 / 917.5 / 3,473.4 |
  | 100,000 | 21.0 / 3,473.4 / 11,272.2 | 20.0 / 2,064.4 / 5,111.8 | 108.5 / 4,849.7 / 14,942.2 |

On this machine the medians of the two transports are within a few
microseconds of each other up to 20,000 requests a second; at 50,000
io_uring's is twice epoll's and at 100,000 more than five times (108.5
against 20.0 µs). Whether that is the transport or the shared CPUs is
for the run with a core per program to say.

### 13. Tick store: differences in variable-length integers, against zstd

Status: accepted
Date: 2026-10-16

Hypothesis
  A tick is one change of a security's best bid or offer, 34 bytes at full
  width. `tick_store profile` on the day (174,890,436 ticks over 8,695
  securities) says what a codec can count on: 35.1% of ticks are for the same
  security as the tick before; a tick usually changes one size (bid size
  alone 39.1%, ask size alone 38.7%); 99.5% of price changes are whole cents,
  and 84% of size changes whole round lots; 98.5% of ticks come within
  2.1 ms of the one before. Flags for what changed, zigzag varints for the
  differences, a same-security bit and cents and lots scaling should land
  near 7 bytes a tick, against 8.3 without the scaling on generated data.

Change
  `DeltaCodec`, as written.

Before / After
  The whole day, 4,096 ticks a block (`tick_store record`):
  | | file bytes | bytes a tick | |
  |---|---|---|---|
  | raw | 5,949,094,678 | 34.02 | |
  | raw through `zstd -3 -T0` | 1,535,924,918 | 8.78 | 23 s on top of recording |
  | delta | 1,277,149,414 | 7.30 | recorded in 254 s, raw in 259 s |
  | raw through `zstd -19 -T0` | 1,203,014,361 | 6.88 | 1,645 s on top of recording |

  `tick_bench --file` on the first 20,000,000 ticks (04:00 to 10:01), five
  runs: at 4,096 ticks a block delta is 7.23 bytes a tick and writes in
  56.3 ns and reads in 49.1 ns a tick, against raw's 106.3 and 102.5; at
  65,536 ticks a block it is 6.63, at 256 it is 8.19.

Correctness
  `tick_store verify` on both stores: 174,890,436 ticks, every block's
  checksum good, and the stream equal to the book's (hash `73b11ec0681d1994`).

Verdict
  Accepted: 4.7 times smaller than raw, 17% smaller than zstd's fast setting
  and 6% larger than its slowest, which took 27 minutes. It also reads and
  writes about twice as fast as raw, plausibly because it moves a fifth of
  the bytes. And a query decodes only the blocks it needs, where a file
  through zstd has to be decompressed before it can be queried at all.

Why
  The profile is the design's justification, and the day bore it out: the
  generated market of the compression test gave 7.0 bytes a tick, the real
  day 7.30. On generated data the cents and lots bits were worth 1.3 bytes a
  tick (8.3 to 7.0); this run did not measure the codec without them.
  Larger blocks help (fewer resets of the per-security state) but make a
  query decode more: at 65,536 ticks a block, a query covering a thousandth
  of the benchmark store's time decodes 1.29 blocks, about 85,000 ticks, to
  hand over 18,777.
