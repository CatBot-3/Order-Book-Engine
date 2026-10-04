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
   is void whatever the timing says.
4. Write the entry yourself, in your own words, from your own runs.

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

## Baseline

The reference build: `std::unordered_map` order store, `std::map` price levels.
Nothing here has been measured yet.

| Workload | msgs/s (median) | range | mean ns | p50 | p99 | p99.9 | max | cache misses / msg | timer cost p50 |
|---|---|---|---|---|---|---|---|---|---|
| `parse` | | | | | | | | | |
| `book` | | | | | | | | | |

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
  before measuring.

Change
  What was changed, in one or two sentences.

Method
  Anything beyond the standard method. Otherwise "standard".

Before
  msgs/s, p50, p99, p99.9, cache misses per message (median and range).

After
  The same columns.

Correctness
  Hash equals the reference: yes | no.

Verdict
  Accepted or rejected, and the size of the effect.

Why
  What actually happened and how I know. If the hypothesis was wrong, what the
  numbers showed instead.
```

## Backlog

Candidates from the project spec, roughly in order of expected payoff.

| # | Experiment | Status |
|---|---|---|
| 1 | Order store: `std::unordered_map` to a pre-sized open-addressing flat table | |
| 2 | Order store: direct index by reference number. Measure the distribution of reference numbers first | |
| 3 | Price levels: `std::map` to a sorted contiguous vector, best price at the back | |
| 4 | Pool allocator for orders | |
| 5 | Struct layout: shrink the order record, split hot and cold fields | |
| 6 | Software prefetch of the next message's order slot | |
| 7 | Dispatch: `switch` against a jump table against a length table | |
| 8 | Huge pages for the order store | |
| 9 | Symbol filter: process only a watch list | |
| 10 | Link-time and profile-guided optimization | |

## Entries

*(none yet)*
