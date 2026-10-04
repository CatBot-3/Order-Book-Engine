# Benchmark method

A number without a method is not worth quoting. This file defines what
`replay_bench` measures, what it leaves out, and what has to be true before a
number from it goes into the README, the optimization log or a resume.

It was written before any optimization, so the first number recorded is an
honest baseline.

## 1. What is run

```sh
cmake --preset release && cmake --build --preset release
scripts/run_bench.sh data/<file> <label>          # wraps the line below and saves the output
build/release/bench/replay_bench data/<file> --cpu 2 --label <label>
```

Two workloads, chosen with `--handler`:

| Workload | Does | Use it for |
|---|---|---|
| `book` (default) | framing, type dispatch, decode, and the book update for every message | the headline numbers |
| `parse` | framing, type dispatch, and decode into a checksum | separating feed cost from book cost |

`book` is `BookManager<OrderStore, PriceLevels>` with no listener attached.
Detecting a best-bid-and-offer change (reading the best level of each side and
comparing) is inside the measurement; delivering it to a consumer is not,
because there is no consumer yet. That changes in phase 6 and this file must
change with it.

## 2. What is inside the timed region

Each run makes two passes over the file. Both start from a freshly constructed
handler, so every pass builds the book from empty.

**Pass 1, throughput.** One `steady_clock` read before the loop and one after.
Inside:

1. reading the two-byte length prefix and bounds-checking it;
2. the type switch and the length-for-type check;
3. decoding the message;
4. the handler: for `book`, the order-store lookup, the level update and the
   best-bid-and-offer comparison.

`msgs/s` and `mean ns` come from this pass. There is no per-message clock in
it, so they are not inflated by measurement.

**Pass 2, latency.** The same loop with one clock read per message. A sample is
the time from the previous clock read to this one. It therefore covers items 1
to 4 for one message **plus one clock read and one histogram update**. The
percentiles come from this pass.

**Outside both:** opening and reading the file, copying it into memory,
validating the stream, constructing and destroying the handler (including
freeing every order at the end), the correctness check, and all reporting.

## 3. Reading the latency numbers

1. **The timer's own cost is reported, not subtracted.** `timer cost` in the
   report is the p50 and p99 of the latency loop with the message removed: two
   consecutive clock reads and a histogram update. A per-message p50 of 60 ns
   with a timer cost of 12 ns means roughly 48 ns of work. Quote both. Do not
   publish a subtracted figure as if it were measured.
2. **Percentiles are bucketed upwards.** The histogram is exact below 64 ticks
   and within 1/32 (about 3%) above that, and it reports the upper edge of the
   bucket. A percentile is never lower than the truth.
3. **`max` is exact** and is one sample. It is usually an interrupt, a page
   fault or a hash table growing, not typical behaviour. Report it, and look at
   p99.9 for the tail that repeats.
4. **`mean ns` and `p50 ns` come from different passes** and will not agree
   exactly. If p50 minus the timer cost is far from the mean, something is
   skewed and worth understanding before quoting either.
5. **The first samples are cold.** There is no separate warm-up inside a pass:
   a pass is a whole day from an empty book, and an empty book warming up is
   part of that day. Warm-up *runs* (below) take care of the process-level
   effects.

### The clock

`--clock tsc` (the default on x86) reads the CPU's timestamp counter with
`rdtsc` and converts ticks to nanoseconds with a factor calibrated against
`steady_clock` over 200 ms at start-up.

1. `rdtsc` is not serializing. The CPU can reorder it with nearby instructions,
   which blurs one sample by a few nanoseconds. Samples are chained (each ends
   where the next begins), so nothing is lost or counted twice.
2. The conversion assumes an invariant TSC. The report says whether the CPU
   advertises one. If it does not, use `--clock steady`.
3. `--clock steady` uses `clock_gettime` through the vDSO. It costs about twice
   as much per read, which the `timer cost` line will show.

## 4. Controls

| Control | How | Why |
|---|---|---|
| Input in memory | The file is copied into anonymous memory before any clock starts. `--no-copy` maps the page cache instead, for machines without the RAM | Disk I/O and page-cache eviction must not land in a timed pass |
| Stream validated first | One untimed parse of the whole file | The timed loops never take an error path |
| Warm-up runs | `--warmup 1` by default, unmeasured | The first run pays for first-touch page faults in the allocator and cold instruction caches |
| Pinning | `--cpu N` | A migrated thread arrives at cold caches; that shows up as tail latency that is not the code's |
| Which CPU | Not CPU 0 if there is a choice | Interrupts are often routed there |
| Frequency policy | Set the `performance` governor if the machine allows it. The report prints the governor it found | A CPU changing frequency mid-run changes the numbers |
| Quiet machine | Close browsers, IDE indexers, anything heavy | They compete for cache and memory bandwidth even on other cores |
| Build | The `release` preset only. The tool warns if assertions or sanitizers are on | Debug and sanitizer builds are several times slower and not representative |

## 5. Repetition

At least five measured runs (`--runs 5` is the default and the tool warns below
it). The report gives every run, then the **median**, the **minimum** and the
**maximum** of each column, and the throughput spread as a percentage of the
median.

Report the median with the range. Never the best run alone.

**The baseline is trustworthy when two consecutive invocations agree within a
few percent.** If the spread is above about 5%, fix the environment before
believing a comparison: an optimization worth 8% cannot be seen through 12% of
noise.

## 6. What the report records

So that a number can be reproduced or challenged later, every report states:
file name, size and message count; workload; CPU model, logical CPU count,
memory; whether a hypervisor is present and whether it is WSL; pinned CPU;
governor; kernel; compiler and version; build type and flags; clock and its
calibration; timer cost; and, for `book`, the correctness line.

`--json FILE` writes the same data in a form scripts can read.
`scripts/run_bench.sh` saves both, plus the git commit, under `results/`.

## 7. Correctness is part of the report

Before timing `book`, the tool replays the file once, untimed, with the hashing
listener, and prints whether every invariant held and the hash of the
best-bid-and-offer stream.

A speed number is only worth anything next to proof that the book was right. In
phase 4 the rule is: **an optimized build's hash must equal the reference
build's hash on the same file, or its timings are discarded.**

Each run also produces a digest of the handler's final state. If two runs
disagree the tool says so; that would mean the workload is not deterministic.

## 8. Hardware counters

The spec asks for cycles, instructions, cache misses and branch misses "through
`perf stat`". `perf stat ./replay_bench` counts the whole process, including
loading several gigabytes and building the report, so "cache misses per
message" from it would divide by the wrong thing.

`replay_bench` instead opens the same kernel counters itself
(`perf_event_open`), enables them immediately before pass 1 and disables them
immediately after. The per-message columns (`cyc/msg`, `ins/msg`, `cmiss/msg`,
`bmiss/msg`) are those counts divided by the messages processed while counting.
User-space only; kernel and hypervisor time are excluded.

`PERF_STAT=1 scripts/run_bench.sh ...` additionally runs once under real
`perf stat` as a whole-process cross-check.

Counters are often unavailable: under WSL2, in most virtual machines and
containers, or when `/proc/sys/kernel/perf_event_paranoid` is above 2. The tool
then says so and reports timings only. It does not guess.

## 9. Known limits and distortions

1. **Virtual machines.** WSL2 and cloud VMs add scheduling noise, make TLB
   misses more expensive and usually hide hardware counters. Numbers taken
   there must be labelled as such. Final numbers are better taken on native
   Linux.
2. **Allocator state carries across runs.** Each pass frees its book at the
   end, so later runs allocate from warm free lists. This affects the reference
   build (one allocation per order and per level) more than a pooled one. It is
   one reason the first run is discarded.
3. **Single thread.** One core, no contention. Phase 6 measures the pipeline.
4. **One file is one day.** A quiet day and a volatile day have different
   message mixes and book depths. State which file was used.
5. **Synthetic data is not evidence.** `itch_synth` output is for checking that
   the harness runs. Its order flow is random, so cache behaviour on it says
   nothing about real data. No number measured on it belongs in the README.
6. **`-march=native` changes the result** and makes the binary non-portable. It
   is off by default (`OBE_NATIVE`). If it is turned on, the flags line in the
   report shows it; record it as an experiment, not as a silent default.

## 10. Before quoting a number

1. Release build, no warnings at the bottom of the report.
2. Real data, and the file is named.
3. At least five runs; median and range given.
4. Spread within a few percent, or the noise is stated.
5. Machine, compiler and flags given.
6. Timer cost given next to any percentile.
7. For `book`: invariants zero and the hash matches the reference.
8. If it was measured in a VM or WSL2, it says so.

## 11. Micro benchmarks

`build/release/bench/micro_bench` (Google Benchmark) times single operations:
a big-endian load, one decode, a parse of a synthetic stream, a histogram
update, a clock read.

They are for answering "did this small change make this small thing faster?".
An operation measured alone in a tight loop, with warm caches and a trained
branch predictor, is cheaper than the same operation inside a real replay. The
replay is the number that counts.

One lesson is already recorded in the code: `BM_HistogramRecord` first reported
0 ns, because the compiler folded the whole loop away. A result that looks too
good is a reason to read the generated code, not to celebrate.
