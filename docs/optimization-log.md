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
ctest --preset debug -L needs-your-code
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

Fill this in once, and again whenever it changes.

| | |
|---|---|
| Machine | |
| CPU | |
| Memory | |
| OS / kernel | |
| Native Linux, WSL2 or VM | |
| Compiler | |
| Flags | |
| Data file | |
| Uncompressed size | |
| Messages | |
| Pinned CPU, governor | |
| Hardware counters available | |
| Transparent huge pages setting | |

## Feed profile

From `feed_profile` on the data file. These are the measurements the
hypotheses below should cite.

| | |
|---|---|
| Most orders resting at once | |
| Reference-number density (orders added / span) | |
| New reference above all earlier ones | |
| Levels on a side when updated: median / 99th / largest | |
| Updates at the best price | |
| Updates within 5 levels of the best | |
| Updates 64 or more levels deep | |
| Share of order messages in the busiest 10 securities | |

## Baseline

The reference build: `std::unordered_map` order store, `std::map` price levels.
Nothing here has been measured yet.

| Workload | msgs/s (median) | range | mean ns | p50 | p99 | p99.9 | max | cache misses / msg | timer cost p50 |
|---|---|---|---|---|---|---|---|---|---|
| `parse` | | | | | | | | | |
| `book` (`--impl reference`) | | | | | | | | | |

Reference best-bid-and-offer hash for this file: ``

Two consecutive invocations agreed within: `%`

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
means the harness can already run the experiment; what is left is the part
that is yours.

| # | Experiment | What is ready | What is yours | Status |
|---|---|---|---|---|
| 1 | Order store: `std::unordered_map` to a pre-sized open-addressing flat table | `--impl flat-store`, `--reserve`, contract tests, differential check | `include/obe/book/flat_order_store.hpp` | |
| 2 | Order store: direct index by reference number | `feed_profile` reports the density and ordering of reference numbers | Decide from the profile whether it is viable. If the span is far larger than the peak of resting orders, log it as rejected with those two numbers and do not build it | |
| 3 | Price levels: `std::map` to a sorted contiguous vector, best price at the back | `--impl vector-levels`, contract tests, differential check; `feed_profile` reports where updates land | `include/obe/book/vector_price_levels.hpp` | |
| 4 | Pool allocator for orders | `--impl pooled` (the reference containers with pooled nodes), the allocator adapter, pool tests | `include/obe/util/pool.hpp` | |
| 5 | Struct layout: shrink the order record, split hot and cold fields | `feed_profile` prints the record sizes | The layout inside `FlatOrderStore` (keys and records together or apart) | |
| 6 | Software prefetch of the next message's order slot | `replay_bench --prefetch` reads one message ahead and calls `prefetch(id)` on the store | A `prefetch` member on `FlatOrderStore` | |
| 7 | Dispatch: `switch` against a jump table against a length table | Nothing yet. The parser has one dispatcher | Ask for the alternative dispatchers when you reach this | |
| 8 | Huge pages for the order store | `util::HugePageBuffer`; the report prints the system setting and the huge-page bytes in use | Back the flat table's slot array with it | |
| 9 | Symbol filter: process only a watch list | `replay_bench --symbols A,B,C` | Choose watch lists of different sizes and plot cost against the share of messages applied | |
| 10 | Link-time and profile-guided optimization | `release-lto` preset; `scripts/pgo_build.sh` | Run them. Expect little from LTO: every app is a single translation unit. Say what the training data for PGO was | |

`--impl flat-vector` combines experiments 1 and 3 for the cumulative row in the
README's results table.

## Entries

*(none yet)*
