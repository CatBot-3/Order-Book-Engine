# Design

How the code is put together and why. Measurements live in
[`benchmark-method.md`](benchmark-method.md) and
[`optimization-log.md`](optimization-log.md); this file is about structure.

## 1. Data flow

```
 file (mmap) ──> FrameReader ──> ItchParser<Handler> ──> BookManager<Store, Levels, Listener>
                 length prefix   type switch, decode      order store + one Book per locate
                 bounds check    length-per-type check             │
                                                                   └──> Listener::on_bbo
                                                                        (BboHasher, recorder, ...)
```

Every arrow is a direct, inlinable call. The handler, the two book containers
and the listener are template parameters, so there is no virtual dispatch
between a byte in the file and the book update it causes.

## 2. Feed layer (`include/obe/feed`)

### 2.1 One field list per message

Each message struct in `messages.hpp` lists its wire fields once, in wire
order, in a `visit()` function. Three visitors in `codec.hpp` walk that list:

| Visitor | Produces |
|---|---|
| `Reader` | `decode<M>(bytes)` |
| `Writer` | `encode(m, bytes)`, used by the test fixtures now and by the engine's market data in phase 5 |
| `Sizer` | `wire_size(m)`, evaluated at compile time |

**Why:** a decoder and an encoder written separately can drift apart, and a
size table written separately can disagree with both. With one list they
cannot. The sizes are then `static_assert`ed against the 23 sizes in the
Nasdaq specification, so a field that is missing, extra or the wrong width
stops the build.

**What a single list cannot catch:** two adjacent fields of the same width in
the wrong order. Decoder and encoder would agree with each other and both be
wrong. That is why the decoder tests build their input with `tests/support/wire.hpp`,
which writes values at absolute offsets copied from the specification and
shares no code with the codec.

### 2.2 Decode in place, with `memcpy`

Fields are read straight from the mapped file with `load_be<T>(ptr)`: a
`memcpy` into a local followed by a byte swap.

**Why not cast the buffer to a packed struct:** ITCH fields sit at odd offsets
(the order reference of an Add Order starts at byte 11). Reading a `uint64_t`
through a pointer to that address is a misaligned access, which is undefined
behaviour whether or not the CPU tolerates it. A fixed-size `memcpy` is defined
for any address and compiles to the same single load.

The 48-bit timestamp is assembled from a 2-byte and a 4-byte load. Loading
eight bytes and masking would read two bytes past the field, and past the end
of the buffer for a 12-byte message that ends the file. A test places a
timestamp in an exactly-sized heap block so AddressSanitizer would catch that.

### 2.3 Framing and dispatch are separate

`FrameReader` walks the two-byte length prefixes and is the only code that
proves a message lies inside the buffer. `ItchParser::dispatch` takes one
message, checks its length against the size its type must have, decodes it and
calls the handler.

**Why split them:** a MoldUDP64 packet (phase 7) carries the same messages with
different framing. It will reuse `dispatch` and replace only the reader.

### 2.4 Compile-time dispatch

`ItchParser<H>` is constrained by the `ItchHandler` concept and calls
`handler.on_add(msg)` and so on directly. A handler inherits empty defaults
from `HandlerBase` and declares only what it needs.

**Why:** the handler type is known at compile time, so a virtual call per
message would buy nothing and would stop the compiler inlining the book update
into the parse loop.

**A limit worth knowing:** the concept catches a callback with the wrong
parameter type. It cannot catch a misspelled name, which silently falls back
to the empty default. The scenario tests are what catch that.

### 2.5 Error policy

Input is untrusted. The parser stops at the first problem and returns a
`ParseStatus` with the offset of the bad record and the number of messages
delivered before it.

| Status | Meaning |
|---|---|
| `TruncatedLength` | fewer than two bytes where a length prefix should be |
| `TruncatedMessage` | the prefix promises more bytes than remain |
| `UnknownType` | the type byte is not an ITCH 5.0 message type |
| `LengthMismatch` | the length is not the one the specification gives for the type |

An unknown type is an error, not something to skip. Skipping would let a
corrupt stream keep "parsing" by luck.

**Not validated:** field values. A side byte that is neither `B` nor `S`, or a
symbol with unprintable characters, is passed through as it is. The book treats
any side other than `B` as a sell.

## 3. Book layer (`include/obe/book`)

### 3.1 Shape

```
BookManager
 ├─ Store                 one for the whole feed: reference number -> {price, shares, side}
 ├─ Book[65536]           flat array indexed by stock locate
 │    ├─ Levels bids      price -> total shares, best = highest
 │    └─ Levels asks      price -> total shares, best = lowest
 └─ Listener              told whenever a book's best bid or offer changes
```

**One order store, not one per book.** Reference numbers are unique across the
feed and an Order Executed message names only the reference number, so the
lookup should not need the book first. It also keeps the hot structure in one
place, which matters for the phase 4 experiments (pre-sizing, huge pages).

**A flat array of books.** Locate codes are 16-bit, so `books_[locate]` needs
no hash and no bounds check. The cost is memory for all 65536 entries whether
used or not; a real day uses under ten thousand.

**The feed-side book has no queues.** A level is a price and a total. ITCH says
which order executed, so the feed side never needs to know who is first at a
price. The matching engine (phase 5) does, and gets its own structure. Treating
the two as one would make the feed side pay for a queue it never reads.

### 3.2 The two contracts

`concepts.hpp` defines `OrderStoreLike` and `PriceLevelsLike`. `BookManager` is
written against those and nothing else.

**Why concepts here:** the reference containers and every phase 4 replacement
satisfy the same contract. The book logic, the scenario tests and the
differential test are then written once. A new container is added to the type
lists in `tests/support/implementations.hpp` and is judged by exactly the tests
the reference passed.

Two parts of the contract are deliberately weak:

1. `find()` returns a pointer that is valid only until the next insert or
   erase. `std::unordered_map` gives more than that, but an open-addressing
   table that grows does not, and `BookManager` must not have to change when
   the container does. `on_replace` copies the old record before erasing for
   this reason.
2. `best()` returns a `Level` by value, not a pointer into the container, so an
   implementation is free to store levels however it likes.

### 3.3 Message handling

| Message | Effect |
|---|---|
| `A`, `F` | insert the order; add its shares to the level |
| `E` | look the order up; take shares off the order and its level; count as volume; erase the order if nothing is left |
| `C` | as `E`, but counted as volume only when printable. The shares come off the order's display level, not the execution price |
| `X` | as `E`, never volume |
| `D` | take the order's remaining shares off its level; erase it |
| `U` | read the side from the original; remove the original; insert the new order under the new reference number on the same side |
| everything else | nothing. `BookManager` does not override those callbacks, so `P` and `Q` prints cannot reach the book |

A replace publishes once, after both halves. The instant between the removal
and the re-add was never a displayed state.

An order that executes or cancels down to zero is erased. ITCH sends no Order
Delete for it.

### 3.4 Best bid and offer

After each book-changing message the manager reads the best level of each side
and compares with what it last published for that book. If either price or
either size differs, the listener gets a `BboUpdate` carrying the locate, the
timestamp of the message and the new values. An empty side is quantity 0.

`BboHasher` folds each update into a per-security hash. Two book
implementations that produce the same hashes published identical update
streams. That is the check for success criterion 3 and for determinism.

### 3.5 Invariants and statistics

These are kept apart on purpose.

**Invariant counters** (`Counters`, plus `audit()`): must all be zero after a
full day.

| Counter | Meaning |
|---|---|
| `unknown_order` | a message named an order that is not resting |
| `overfill` | an execution or cancel exceeded what the order had left ("negative remaining shares") |
| `duplicate_order` | an add or replace introduced a reference number that is already live |
| `zero_shares` | a message with a share count of zero |
| `level_mismatch` | the levels refused a removal the order store said was valid |
| `audit().empty_levels` | a level with zero quantity still present |
| `audit()` shares | the sum of all level quantities must equal the sum of all resting orders' shares |

A violation is never fatal. The message is skipped or clamped, the counter
records it, and the replay continues so one run shows the full extent of a
problem. `book_replay` exits non-zero if any of them is not zero.

**Statistics** (`Stats`): expected to be non-zero on real data. See the next
section.

## 4. Locked and crossed books

A book is *locked* when the best bid equals the best ask and *crossed* when the
best bid is above it.

**How the code treats them:** as facts to record, not errors. The feed-side
book mirrors what the exchange displayed. It holds a locked or crossed state
exactly as it holds any other, publishes it like any other update, and counts
it in `Stats`, split by the security's trading state (from the Stock Trading
Action messages) at that moment:

| Statistic | Meaning |
|---|---|
| `locked_while_trading`, `crossed_while_trading` | the update happened while the state was `T` |
| `locked_while_not_trading`, `crossed_while_not_trading` | any other state: halted `H`, paused `P`, quotation only `Q`, or none seen yet |

Nothing asserts that a book is uncrossed, and the matching engine's "never
crossed" rule (phase 5) does not apply here.

**What to expect, to be checked against the data:** during continuous trading
an incoming order that could trade does trade, so it is reported as executions
against resting orders and is never added at a crossing price. Orders can still
be entered while a security is halted or in a quotation-only period, and they
rest without matching until the auction that ends the halt. So crossed and
locked updates should appear only outside state `T`.

**Observed on the real file:** *not yet run.* After the first full-day replay,
fill this in from `book_replay`'s output:

| | state `T` | any other state |
|---|---|---|
| locked updates | | |
| crossed updates | | |

If the `T` column is not zero, find out why before going further (which
securities, what time of day, what message caused it) and write the answer
here. `book_view <file> <symbol> --at <time>` shows the book at that moment.

## 5. Testing

| Layer | Where | What it pins down |
|---|---|---|
| Unit | `tests/feed`, `tests/util` | each decoder at specification offsets, the 48-bit timestamp, framing errors, formatting |
| Random input | `tests/feed/parser_test.cpp`, `fuzz/` | truncation at every byte, random and corrupted bytes, decode/encode identity |
| Container contract | `tests/book/order_store_test.cpp`, `price_levels_test.cpp` | each container against a `std::map` model over long random sequences |
| Scenario | `tests/book/book_manager_test.cpp` | one rule of section 4.5 of the spec per test |
| Golden | `tests/book/book_replay_test.cpp` | a hand-built byte stream with a hand-worked book |
| Differential | same file | `BookManager` against `NaiveBook`, update for update |
| Differential | `tests/book/differential_test.cpp`, `scripts/diff_books.sh` | each optimized book against the reference book, security by security |
| Integration | `ctest` entries in `tests/CMakeLists.txt` | the real apps on a generated file, against the independent Python counter |
| Engine scenario | `tests/engine/engine_scenario_test.cpp` | every order type and every edge of the engine contract: reports, market data and the book left behind |
| Engine property | `tests/engine/engine_property_test.cpp` | never crossed, shares conserved, price-time order, determinism, and agreement with an oracle, over seeded random flow |
| Round trip | `tests/engine/round_trip_test.cpp`, `scripts/flow_round_trip.sh` | the engine's feed, through the feed handler, rebuilds the engine's book |
| Engine differential | `tests/engine/engine_differential_test.cpp`, `scripts/diff_engines.sh` | the hand-written engine says exactly what the reference says |
| Queue contract | `tests/util/queue_test.cpp`, `channel_test.cpp` | order, full and empty on one thread; every item once, in order and intact across two; close and cancel |
| Pipeline | `tests/pipeline/pipeline_test.cpp` | three threads publish exactly what one thread publishes, on any queue, with any capacity, on bad input, and when a stage throws |
| Seqlock | `tests/util/seqlock_test.cpp` | a reader never sees a value nobody stored |
| Wire formats | `tests/net/protocol_test.cpp`, `moldudp_test.cpp` | every order-entry message at hand-built offsets; packet bytes; loss, duplication and reordering of packets |
| Gateway | `tests/net/gateway_test.cpp` | partial reads, mid-message disconnects, bad input, slow clients, clients that pause and resume, connection limits and identities, over real loopback sockets |
| Load measurement | `tests/net/open_loop_test.cpp` | due times neither drift nor overflow; a stall is charged to every request due during it, shown against a send-and-wait generator in simulated time |
| The three programs | `scripts/gateway_smoke.sh` | orders in over TCP, market data out over UDP, the subscriber's book consistent and gap-free |
| Simulator scenario | `tests/sim/sim_scenario_test.cpp` | each rule of the fill model on hand-built messages: who is ahead, the three ways a quote trades, refusals, halts, latency, every money figure |
| Simulator property | `tests/sim/sim_property_test.cpp` | over generated markets, after every message: the queue ahead against the test's own copy of the orders, the money recounted from the fills, the book against a plain replay |
| Strategies | `tests/sim/strategy_test.cpp`, `avellaneda_stoikov_test.cpp` | tick rounding, the simple strategies and the volatility estimate against figures worked out by hand; the hand-written strategy against its contract |
| Journal format | `tests/journal/journal_format_test.cpp`, `fuzz/journal_fuzz.cpp` | record bytes built by hand; truncation at every byte, every single-bit error, and the line between a torn tail and corruption |
| Recovery | `tests/journal/recovery_test.cpp` | an engine rebuilt from a journal holds, said and goes on to say exactly what the original did; stopping short anywhere gives the engine as it was then; group commit loses only the unfinished batch |
| Snapshot | `tests/journal/snapshot_test.cpp` | restore then compare as above; each thing a snapshot could forget; damaged and implausible snapshots refused |
| Recovery from files | `tests/journal/journal_file_test.cpp`, `scripts/journal_crash_test.sh` | a torn tail cut and carried on in the same file; the real program killed repeatedly ends with the journal of a run that never was |

`NaiveBook` (`tests/support/naive_book.hpp`) is the oracle: a flat map of
orders that recomputes the best bid and offer by scanning every order after
every message. It is far too slow for real data and very hard to get wrong.

`NaiveEngine` (`tests/support/naive_engine.hpp`) is the same idea for the
matching engine: every resting order of every instrument in one vector, and a
scan of all of it to find who trades next.

All of this runs under AddressSanitizer and UndefinedBehaviorSanitizer in the
`debug` preset, and the `tsan` preset runs it under ThreadSanitizer, which is
the check that matters for the queues and the seqlock (section 9.6).

## 6. Written by hand

The project spec keeps the core data structures hand-written. Their headers
ship with the interface, the contract, design questions and a test suite; the
bodies are left out until they are written.

| Component | File | Status |
|---|---|---|
| Reference order store | `include/obe/book/order_store.hpp` | written |
| Reference price levels | `include/obe/book/price_levels.hpp` | written |
| Flat order table (experiment 1) | `include/obe/book/flat_order_store.hpp` | **to write** |
| Contiguous price levels (experiment 3) | `include/obe/book/vector_price_levels.hpp` | **to write** |
| Pool (experiment 4, and the engine later) | `include/obe/util/pool.hpp` | **to write** |
| Reference matching engine | `include/obe/engine/reference_engine.hpp` | written |
| Matching engine (pooled, intrusive queues) | `include/obe/engine/matching_engine.hpp` | **to write** |
| Mutex queue, spin channel, seqlock, pipeline | `include/obe/util/mutex_queue.hpp`, `spin_channel.hpp`, `seqlock.hpp`, `include/obe/pipeline/` | written |
| Lock-free ring | `include/obe/util/spsc_ring.hpp` | **to write** |
| Gateway, market-data publisher and receiver, load generator | `include/obe/net/`, `apps/` | written |
| Market-making simulator, fill model, simple strategies, volatility estimate | `include/obe/sim/` | written |
| Avellaneda-Stoikov strategy | `include/obe/sim/avellaneda_stoikov.hpp` | **to write** |
| Journal, replay, snapshot, recovery from files | `include/obe/journal/` | written |
| Snapshot support for the hand-written engine | the five `Restorable` functions, in `include/obe/engine/matching_engine.hpp` | optional: see below |

Each "to write" header holds the interface, the questions to settle and where
the measurements that settle them come from. Their tests are built into
second executables (`obe_hand_written_tests` for the containers,
`obe_hand_written_engine_tests` for the engine,
`obe_hand_written_concurrency_tests` for the ring) from the same sources as the
reference tests, and carry the `ctest` label `needs-your-code`. The strategy
has an executable of its own, `obe_hand_written_sim_tests`, under the same
label:

```sh
ctest --preset debug -LE needs-your-code     # everything that is finished
ctest --preset debug -L needs-your-code      # the hand-written parts
```

Until a container is written its tests fail with a message naming the missing
function, and `--impl <name>` in the apps exits with status 4. Record the
decisions you make for each one in sections 6.3 onwards.

The hand-written engine is journaled and recovered by replay as soon as it
matches: `obe_hand_written_journal_tests` and the second crash test run on it
under the same label. Saving it to a snapshot is a further step and nothing
waits on it. It means adding the five functions listed under "Restoring" in
`engine/concepts.hpp`, then pointing the `Impl` alias at the top of
`tests/journal/snapshot_test.cpp` at it, which runs that whole suite on the
new engine. The question worth settling first: when
orders are restored one at a time into intrusive queues of pooled nodes, what
has to be true of the pool and the index afterwards for the engine to be
indistinguishable from one that got there by trading?

### 6.1 Reference order store

`std::unordered_map<OrderId, OrderRecord>` behind the `OrderStoreLike`
contract. The reasoning for each choice is in the header; in short:

1. `insert` uses `try_emplace`: one lookup, never overwrites, and reports a
   duplicate so the book can count it.
2. `find` returns a pointer promised only until the next insert or erase. The
   node-based map gives more, but the contract is set by the weakest container
   that will ever implement it.
3. An optional constructor argument reserves buckets for the expected peak of
   live orders. That removes rehashes; it cannot remove the per-order node
   allocation, which is one of the costs phase 4 goes after.

### 6.2 Reference price levels

`std::map<Price, std::uint64_t>` behind the `PriceLevelsLike` contract, one
object per side.

1. One class serves both sides and branches on a stored flag to pick the front
   (best ask) or the back (best bid). A comparator template parameter or a
   negated key would avoid the branch at the cost of either two types or a
   conversion on every price that crosses the interface. The reference chooses
   whatever is most obviously right.
2. A level that reaches zero is erased, so `best()` can never report a price
   with no shares.
3. `remove` refuses and changes nothing when the level is missing or too
   small, which turns a book inconsistency into a counter instead of a quietly
   wrong book.

Both are deliberately the "slow, obvious" versions. Their costs (a heap node
per order and per level, pointer chasing on every lookup, rehash spikes) are
what the baseline measures and what phase 4 replaces one at a time.

### 6.3 Flat order table

*(yours)*

### 6.4 Contiguous price levels

*(yours)*

### 6.5 Pool

*(yours)*

### 6.6 Matching engine

*(yours)*

### 6.7 Lock-free ring

*(yours)*

### 6.8 Avellaneda-Stoikov strategy

*(yours)*

The spec's list of parts to write by hand names the order store, the price
levels, the queue and the matching loop. This one is not on it. It is left to
write for the same reason those are: it is the part of the simulator that
carries the idea, it is about twenty lines, and a strategy that cannot be
derived at a whiteboard is not one to put a name to. Everything else in the
simulator runs without it.

## 7. Phase 4 structure

Phase 4 swaps containers and measures. The structure that makes each swap safe
and each measurement comparable:

**Named implementations** (`include/obe/book/implementations.hpp`). A book is a
store paired with a level container, and each pairing has a name. Every pairing
differs from the reference in exactly one respect, so one comparison isolates
one effect; `flat-vector` is the exception and exists for the cumulative
result. `book_replay`, `replay_bench` and the tests all select by that name,
so a new pairing is replayable, benchmarkable and tested by adding it to one
list.

**One test suite, built twice.** `tests/support/implementations.hpp` switches
the typed-test lists on a compile definition. The reference build is part of
the passing suite. The hand-written build runs the same contract, scenario and
golden tests against the optimized containers, plus
`tests/book/differential_test.cpp`, which replays generated streams of four
different shapes through an optimized book and the reference and requires
identical update hashes, counters and final books.

**The comparison is enforced, not remembered.** `replay_bench` replays the
reference alongside any other implementation before timing it, and marks the
timings void if the hashes differ. `scripts/diff_books.sh` does the same on a
full day, security by security.

**Measure the premise first.** `feed_profile` reports the facts the experiments
assume: the peak of resting orders, how reference numbers are distributed, how
long sides get, and how far from the best price updates land. It uses the
reference book, and it checks its own level accounting against the book it
profiled.

**The pool is shared.** `util::PoolAllocator` is stateless and every instance
for a node type uses one pool. A pool per container would give each of the
131072 level maps its own slabs. The cost is that pooled containers of one
node type are not safe to use from two threads, which the single-writer design
already rules out.

**Optional hooks are detected, not required.** `BookManager::prefetch` forwards
to the store's `prefetch` if it has one and compiles to nothing otherwise, so
the prefetch experiment needs no change to the contract or to the reference.

## 8. Matching engine (`include/obe/engine`)

### 8.1 Shape

```
 submit / cancel / replace ──> Engine<Reports, MarketData>
                                 │                    │
                 Reports::on_executed ...      MarketData::on_add, on_execute ...
                 to the order's owner          to everybody, anonymous
                                                      │
                                   ItchFeedWriter (bytes)   or   BookManager (directly)
```

The engine is a class template over its two sinks, for the same reason the
parser is a template over its handler: the calls are direct and inlinable, and
a benchmark can plug in sinks that do nothing.

Two audiences hear about every change. The owner gets reports that name the
order and what happened to it. The market gets ITCH messages that describe the
displayed book and say nothing about who. An order that trades the moment it
arrives never appears in the market data as an order: the market only sees the
resting orders it took shares from. That is why a full fill of an incoming
order produces `E` messages and no `A`.

`MarketDataSink` is deliberately a subset of the ITCH handler interface. A
`BookManager` is therefore a market-data sink as it stands, and the engine can
drive a feed-side book with no bytes in between. `ItchFeedWriter` is the other
sink: it encodes each message in the layout of Nasdaq's sample files, so
everything that reads a real day reads the engine's output.

The engine never reads a clock. Every operation takes the time from the
caller and stamps its output with it. It also assigns the order ids and match
numbers itself, counting up from one. Together those make a run a pure
function of its requests, which is what the differential test and the
before-and-after benchmark both rely on.

### 8.2 The rules, and the ones that are choices

The full contract is the comment at the top of `concepts.hpp`. The parts that
are design decisions and not arithmetic:

1. **Price, then time.** The best-priced resting order trades first; among
   orders at one price, the one that has waited longest. Trades happen at the
   resting order's price, so an aggressive order can do better than its limit
   and never worse.
2. **A replace keeps its place only if it shrinks.** Same price and a size no
   larger: the order keeps its id and its position, and the market sees `X`.
   A new price or a larger size: the order goes to the back under a new id,
   and the market sees `U`. If an order could grow in place, one share would
   be enough to hold a position in the queue for later.
3. **A replace that trades is published as `D`, `E`..., `A`.** The market data
   has no way to show an order that executes on arrival, so there is nothing
   for a `U` to point its new reference number at. The old order is deleted
   and whatever survives the trades is added afresh.
4. **Fill-or-kill is decided before the first trade.** A trade cannot be
   taken back once it is published, so the engine first sums the resting
   shares at acceptable prices. Level totals make that a walk over levels, not
   orders.
5. **A market order never rests.** With no price there is no level to put it
   on. What it cannot fill is cancelled, with a reason that says the book ran
   out.
6. **A refused request changes nothing and uses no id.** The checks run in a
   fixed order and the first failure is the one reported. The order of the
   checks is part of the contract only because two engines have to agree on
   it.
7. **Self-trading is allowed.** An owner's order can trade with another of
   their own. Preventing it is on the spec's stretch list.

Left out on purpose: auctions, halts, price bands, lot-size rules, hidden and
pegged orders. Every instrument trades continuously from the moment it is
opened.

### 8.3 Two engines

| Name | File | Structure |
|---|---|---|
| `reference` | `reference_engine.hpp` | `std::map` of levels, a `std::list` of orders per level, `std::unordered_map` from id to a list iterator |
| `pooled` | `matching_engine.hpp` | intrusive queues of orders from `util::Pool`; **written by hand** |

`flow_gen --engine NAME` and `engine_bench --engine NAME` select one. The
reference allocates up to three nodes for an order that rests (list, index,
and a tree node if the price is new) and frees them when it leaves. That is
the cost the hand-written engine is there to remove, and `engine_bench` is how
the difference is measured.

`engine_bench` records the requests once, as a tape, by running the generator
through the reference engine. It then replays the tape into the engine under
test: once with hashing sinks, to check that the engine says what the
reference says, and then for timing. Requests that fill the book to its target
size are applied before the clock starts, so the measurement is a full book in
steady state.

### 8.4 How it is tested

A matching engine that is subtly wrong still produces plausible output, so the
tests look at it from several independent directions:

1. **Scenarios** state, for each order type and edge, the exact reports, the
   exact market data and the exact book afterwards.
2. **Properties** over seeded random flow are each checked from a different
   place: the book is never crossed (from the engine's own book), shares are
   conserved (from the reports alone), trades follow price then time (from the
   market data alone, as an outsider would see it), and a seed gives the same
   bytes twice.
3. **The oracle.** `NaiveEngine` implements the same contract with no data
   structure at all. The scenarios run against it too, and the property suite
   requires the engine under test to match it request by request.
4. **The round trip.** The published bytes, parsed by the phase 2 feed handler,
   must rebuild the engine's book. The comparison is made at checkpoints
   through the run, not only at the end.
5. **A test of the tests.** `TheFlowsReachEveryPath` fails if the random flows
   stop producing every reject reason, every cancel reason, both kinds of
   replace, sweeps, and replaces that trade. Without it the properties could
   pass on flow that never visits the hard cases.
6. **The differential test** holds the hand-written engine to the reference's
   output: market data byte for byte, reports one by one.

### 8.5 The order-flow generator (`include/obe/gen/order_flow.hpp`)

`OrderFlow` produces requests, not messages. Each symbol has a mid price that
takes a random walk; most orders are placed a few ticks behind it, a minority
reach across it, and cancels and replaces pick a random resting order. The
number of resting orders hovers around a target. A small share of requests is
wrong on purpose, to exercise the rejects.

It learns which orders are resting the way a participant would: it is a report
sink, and whoever runs the engine routes the reports to it. That is also a
check on the reports: `flow_gen` fails if the orders the generator believes are
resting are not the orders in the book.

`flow_gen` runs the generator through an engine and writes the feed to a file.
It replaces `itch_synth` wherever the shape of the book matters: trades take
the best price and the oldest order, and the two sides never cross.

It is still not a market. There are no participants with intentions, no
correlation between symbols, and executions are a larger share of the messages
than on a real day (`itch_stats` shows the mix of any file). Numbers measured
on it compare one build of this code with another. They say nothing about
Nasdaq.

## 9. Threads and queues (`include/obe/util`, `include/obe/pipeline`)

### 9.1 Two contracts

`queue_concepts.hpp` separates two things that are usually tangled together.

A **`BoundedQueue`** has `try_push` and `try_pop` and never waits. This is all a
lock-free ring is, and keeping the contract that small keeps the hand-written
structure small enough to reason about completely.

A **`Channel`** can be waited on: `push` waits for room, `pop` waits for an
item, the producer can `close` it to say "no more", and anybody can `cancel` it
to say "stop now". A pipeline stage needs all four. How a channel waits is its
own business.

`SpinChannel<Queue>` builds the second out of the first by retrying. So the
waiting policy, the end-of-stream flag and the cancellation flag are written
once, tested once, and shared by every queue.

One detail in it is easy to get wrong. A consumer that finds the queue empty
and then sees "closed" cannot conclude the stream is over: the producer may
have pushed its last item and closed between the two observations. `pop` reads
the closed flag first and looks at the queue second, so that an empty queue
seen after "closed" really is final. `TheLastItemBeforeACloseIsNeverLost`
tests exactly this, two thousand times over.

### 9.2 Three channels, each one change from the next

| `--queue` | Lock | A thread that has to wait |
|---|---|---|
| `mutex` | `std::mutex` | sleeps on a condition variable |
| `mutex-spin` | `std::mutex` | spins |
| `ring` | none (`SpscRing`, **written by hand**) | spins |

The spec asks for the ring to be compared with a mutex queue. Comparing only
those two would mix two differences: the lock, and the sleep. The middle row
separates them. `mutex` against `mutex-spin` is what sleeping costs;
`mutex-spin` against `ring` is what the lock costs.

Spinning is not free either. A spinning thread keeps a core at full power while
it has nothing to do, and on a machine with fewer free cores than threads it
takes time from the very thread it is waiting for. `util::Backoff` spins a
bounded number of times with the CPU's pause hint and then starts yielding,
which keeps oversubscribed machines (CI, ThreadSanitizer runs) making progress.

### 9.3 The pipeline

```
 parser thread ──(BookEvent)──> book thread ──(BboUpdate)──> consumer thread
 framing, decode                order store, levels           hash, statistics
                                       │
                                       └──> TopOfBookBoard (a Seqlock per locate) ──> any reader
```

`run_pipeline<Channel, Impl>(bytes)` is the single-threaded replay cut at two
points. Each stage owns its data outright; the only things two threads both
touch are the queues and the board. The book logic is the phase 2
`BookManager`, unchanged.

What crosses the first queue is a `BookEvent`: the decoded message flattened
into 40 bytes of plain integers. Decoding on the parser thread is a choice. The
alternative is to pass a pointer to the undecoded bytes and decode on the book
thread, which would leave the first thread with almost nothing to do. Messages
that cannot change a book are dropped before the queue.

The result does not depend on how the threads interleave: each queue keeps
order, each stage has one input, and no stage reads a clock. So the consumer's
hash must equal the single-threaded replay's, and that equality is the
pipeline's correctness test. `pipeline_bench` checks it on every run and voids
its timings if it fails.

A stage that throws cancels both queues, which releases its neighbours from
whatever wait they were in; every thread is joined, and the exception is
rethrown on the caller's thread. Without that, one failing stage would leave
the other two waiting for ever.

### 9.4 Seqlock and the board

The queue to the consumer carries every update, in order, to one reader. The
board answers a different question for any number of readers: what is the top
of book of this security now? `Seqlock<T>` lets the book thread publish it
without ever waiting for a reader: the writer bumps a sequence number to odd,
writes, and bumps it to even; a reader copies the value and retries if the
sequence was odd or changed while it copied.

The value is stored as 64-bit atomic words, not as a plain `T`. In the textbook
seqlock a reader copies a plain value while the writer may be changing it and
discards the copy if it was torn. In C++ that copy is a data race, which is
undefined behaviour whether or not the result is used, and ThreadSanitizer
reports it. With atomic words every access is defined, and on x86 the machine
code is the same plain moves.

The writer is wait-free. A reader is only lock-free: it can be made to retry
for as long as writes keep arriving. `try_load` exists for a reader that would
sooner skip a sample than wait.

### 9.5 What is measured

`queue_bench` moves numbered items from one thread to another and times the
whole transfer. The consumer checks every item, so the same program is the
ordered stress run of criterion 6 (`scripts/queue_stress.sh` runs it with one
billion items).

`pipeline_bench` replays one file both ways in one process, single-threaded
and as the pipeline, checks that the two produced the same hash, and prints
the ratio. It also prints how often each stage had to wait on each queue,
which is what explains the ratio: the slowest stage is the one whose input
queue is full and whose output queue is empty.

The pipeline may lose, and the spec says so. A message costs tens of
nanoseconds to process; moving it to another core costs a cache-line transfer
each way. Either outcome goes in the log with the numbers and the CPUs the
threads were pinned to.

### 9.6 How it is tested

1. **The contract, on one thread:** order, full, empty, wrap-around, a model
   comparison on random operations, and what happens to a value whose push was
   refused (nothing: the caller retries with it).
2. **Two threads:** every item arrives once, in order and intact, with the
   queue kept mostly full, mostly empty, and with one slot. The items carry
   fields derived from each other, so a slot read before it was completely
   written is detected.
3. **ThreadSanitizer.** This is the test of the memory ordering, and a stress
   run is not. x86 orders stores and loads more strictly than C++ requires, so
   a ring with every ordering relaxed passes a billion-item run on it without
   error. ThreadSanitizer checks the ordering the code asked for. The `tsan`
   test preset sets `TSAN_OPTIONS=halt_on_error=1`, so a test stops at its
   first race report. Without it a wrong ordering in a loop that runs a
   hundred thousand times produces a report for most of them, and a test that
   should fail in a second runs for many minutes printing them.
4. **The pipeline against the single thread:** same hash for every security,
   same counters, same final book, on streams of both generators, with queues
   of one slot, on truncated and corrupt input, and with a stage that throws.
5. **The seqlock:** readers on other threads check on every load that the value
   is one some store published whole, and that they never go back in time.

The tests are written to fail cleanly when the ring is not there yet or is
wrong: a thread's exception is carried back to the test, and every waiting
loop has a way out.

## 10. Network (`include/obe/net`)

### 10.1 Shape

```
 load_gen ──TCP, order entry──> OrderGateway ──> engine ──> reports ──TCP──> load_gen
 (many connections)             (one thread, epoll)   │
                                                      └──> MoldPacketizer ──UDP──> md_listen
                                                           market data            MoldReceiver
                                                                                  -> BookManager
```

`exchange_server` is the gateway, a matching engine and the publisher in one
single-threaded process. `load_gen` is the client and `md_listen` is a
market-data subscriber. With all three running, an order goes in over TCP, is
matched, is published over UDP, and is rebuilt into a book by the same
`BookManager` that replays a Nasdaq file.

The spec puts this code in `src/net/`. It is in `include/obe/net/` instead,
header-only like everything else, because the gateway is a template over the
engine and there would be nothing left to compile separately.

### 10.2 Order entry (`protocol.hpp`, `order_gateway.hpp`)

**The protocol** is modelled loosely on Nasdaq's OUCH: three requests (enter,
replace, cancel) and five responses (accepted, executed, cancelled, replaced,
rejected), each a fixed size, each starting with a type byte. The messages are
described by the same one-field-list-per-message scheme as the ITCH messages,
so encode, decode and size cannot disagree, and the sizes are pinned by
`static_assert`. What is left out is the session layer OUCH rides on (logins,
heartbeats, sequence numbers, replay after a reconnect): a connection is the
session.

**Framing.** TCP delivers a stream of bytes with no message boundaries. Here
the type byte decides how long a message is. The consequence is that an unknown
type byte is fatal for the connection: nothing says where the next message
starts.

**One thread, driven by epoll.** A request is read, decoded and handed to the
engine in one call; the engine's reports land in the output buffers of the
connections they belong to before that call returns; at the end of each turn of
the loop every buffer that gained something is written, with one `send` per
connection. Nothing is locked because nothing is shared. The cost of that
simplicity is that one core does everything; the threads and queues of section
9 are what would be put between the gateway and the engine to change that.

The three things a TCP server has to get right:

1. **Partial reads.** One `recv` can return half a message, or three and a
   half. Each connection keeps the bytes of a request that has not arrived
   completely, and only whole requests are acted on. In the common case, when
   nothing is left over from the previous read, requests are served straight
   out of the read buffer and nothing is copied.
2. **Slow clients.** A write never blocks. What a socket will not take stays in
   the connection's buffer and the socket is watched for writability. A client
   whose backlog passes a limit is disconnected: it must not be able to grow
   the server's memory, or delay anybody else, by not reading.
3. **Disconnects at any moment.** A connection that closes in the middle of a
   message loses that message and nothing else. Its resting orders are
   cancelled (configurable), in id order so that the market data this
   publishes is deterministic.

Level-triggered epoll is used, not edge-triggered. Edge-triggered saves a few
wake-ups and in exchange every handler must read until the socket is empty or
lose the notification for ever. Level-triggered cannot lose one, and lets a
read stop after a bounded number of bytes so that one flooding client cannot
starve the rest of a turn.

`TCP_NODELAY` is set on every connection. Without it the kernel holds a small
write back while an earlier one is unacknowledged, which can add tens of
milliseconds to a message of a few dozen bytes.

### 10.3 Market data (`moldudp.hpp`, `udp.hpp`)

Packets follow the shape of Nasdaq's MoldUDP64: a ten-byte session name, the
sequence number of the first message, a message count, then length-prefixed
ITCH messages. Every message has a sequence number, so a receiver that keeps
"the number I expect next" can tell exactly what it missed:

| A packet that starts | means |
|---|---|
| at the expected number | the normal case |
| above it | messages were lost: a gap, counted with its size |
| below it | a repeat or a late arrival: the messages already seen are skipped |

A heartbeat (count zero) carries the next sequence number, so a loss at the end
of a burst is noticed without waiting for the next message; the server sends
one after a second of silence. An end-of-session packet lets subscribers stop.

`MoldPacketizer` and `MoldReceiver` contain no sockets, which is what lets the
tests lose, repeat and reorder packets on purpose. `UdpSender` and
`UdpReceiver` put sockets around them; the destination can be a multicast
group, as an exchange would use, or an ordinary address.

**There is no recovery.** Real MoldUDP64 has a second channel on which a
receiver asks for a range of messages again. Here a gap is detected, counted
and reported (`md_listen` exits with status 2), and the subscriber's book is
wrong from then on. For the same reason a subscriber that starts late has
missed the instruments and every resting order and cannot catch up.

### 10.4 Measuring it (`load_gen`)

`load_gen` sends the same seeded order flow that drives the engine in the
tests, over many connections, at a fixed rate, and records how long each
request takes to be acknowledged.

The schedule is fixed in advance: request *i* is due at `start + i / rate`.
Latency runs from that due time to the moment the acknowledgement is read, not
from the moment the request was actually written. The difference is the whole
point. A generator that waits for each answer before sending the next slows
down when the server does: during a one-second stall it sends one request and
records one slow sample, and the thousands of requests that should have been
sent in that second are never sent and never counted. The stall all but
vanishes from the percentiles. This is called coordinated omission. With a
fixed schedule those requests fall due regardless, each is late by as long as
it waited, and the stall appears at its true size.

A generator on a fixed schedule can itself fall behind, and its lateness then
lands in the numbers. The report gives the worst lag behind schedule for each
rate, so that a row where the generator was the bottleneck can be recognised
and discarded.

Acknowledgements are matched to requests without tokens: the gateway answers a
connection's requests in the order they arrived, so each acknowledgement
belongs to the oldest unanswered request on its connection. Executions, and
cancels the client did not ask for, are not acknowledgements and are skipped.

Running several rates gives the latency-against-throughput curve
(`scripts/latency_curve.py` prints it as a table and plots it). It is flat,
then turns sharply upwards at the rate where the slowest part of the path is
saturated. Rows past that knee show where the capacity is; their latencies
depend on how long the run was and are not worth quoting.

**All of this is loopback.** Both ends are on one machine and no packet reaches
a network card. The numbers measure the software path: system calls, the
kernel's TCP stack, the gateway and the engine. They say nothing about a
network, and nothing about a machine where the generator and the server do not
compete for the same cores.

### 10.5 How it is tested

1. **The wire format:** every message at byte offsets built by hand, and the
   mapping to and from the engine's types.
2. **The packet logic, without sockets:** exact packet bytes; then loss,
   duplication, reordering, overlap, truncation at every length, a wrong
   session, a heartbeat that reveals a loss. The property that matters most is
   that a loss is never silent: whenever messages were dropped,
   `complete()` is false and the count of missed messages is exact.
3. **The gateway, over real loopback sockets, on one thread.** The test is the
   client and also turns the gateway's event loop by hand, which is possible
   because nothing in the gateway happens outside `poll()`. That makes cases
   deterministic that are usually tested with sleeps: a request arriving one
   byte at a time, three and a half requests in one write, a disconnect in the
   middle of a message, a byte that is not a request type, a client that never
   reads, a client that stops reading until the gateway's writes are refused
   and then reads everything (every answer must still arrive, once and in
   order), connections over the limit, and a new connection arriving after
   another has left (it must not inherit an identity still in use).
4. **The sockets:** datagram boundaries, a feed round trip over a loopback
   address and over a multicast group (skipped where multicast is not
   available).
5. **The measurement itself** (`open_loop.hpp`, `tests/net/open_loop_test.cpp`).
   The schedule and the matching of acknowledgements are kept apart from the
   sockets so that the claim of section 10.4 can be checked in simulated time:
   due times at rates that do not divide a second neither drift nor overflow,
   and against a server that stalls for one second in ten, the fixed schedule
   records about 900 slow requests and a 99th percentile inside the stall,
   where a send-and-wait generator records one slow sample and a 99th
   percentile equal to the service time.
6. **The three programs together** (`scripts/gateway_smoke.sh`, a `ctest`
   entry): every request acknowledged, the subscriber sees the whole feed with
   no gap and rebuilds a consistent book, the server shuts down cleanly.

## 11. Market-making simulator (`include/obe/sim`)

**This is a simulation with stated assumptions, not evidence of
profitability.** It replays a recorded feed and asks what would have happened
to quotes that were never in the market. The recorded market did not see them
and did not react to them. What it is good for is comparing strategies under
the same assumptions, and seeing adverse selection in numbers.

The spec keeps this phase out of the core, and so does the code: everything is
in `include/obe/sim/`, `apps/mm_sim.cpp` and `tests/sim/`, and nothing in
sections 2 to 10 includes any of it.

### 11.1 Shape

```
 ITCH file ──> parser ──> MarketMakingSim ──────────────> BookManager (unchanged)
                            │  for the quoted security, looks at each
                            │  message before the book applies it
                            ├──> fill model: queue position, fills
                            ├──> strategy: "what do you want resting?"
                            └──> report: fills, position, money, markouts
```

`MarketMakingSim` is an ITCH handler, like `BookManager`, and contains one.
Every message goes on to the real book unchanged; a strategy that never quotes
gives exactly a plain replay, and a test holds it to that. For the one quoted
security the simulator looks at each message first, while the order the
message names is still as it was.

### 11.2 The fill model

A simulated quote rests at a price, behind everything displayed at that price
when it joined.

The spec's outline says the position of a cancel in the queue is unknown and
that an assumption is needed. That is true of a feed that publishes level
totals. ITCH publishes every order, and names the order in every execution,
cancel and delete. So for each real order at our price it is known whether it
arrived before our quote (ahead) or after (behind): the simulator numbers the
real orders of the security in arrival order and remembers the number that was
current when the quote joined. No assumption is needed for displayed orders.

From that, a quote moves up the queue one way and trades three ways:

| What the feed shows | What it means for our quote | Why |
|---|---|---|
| An order **ahead** of us is executed, cancelled or deleted | the queue in front is shorter | it was in front, and it has gone |
| An order **behind** us at our price is executed | we traded, for up to as many shares | priority at a price is by time: whoever traded with it met us first |
| An order at a **worse** price on our side is executed | we traded, for up to as many shares | priority is by price first: the trade went past ours to reach it |
| Our quote is better than every real one on its side, and an order **arrives** on the other side at a price that reaches ours | we traded, for up to its size | in the recorded market it rested; with our quote there it would have met it |

A replaced order is an order leaving and a new one arriving at the back, which
is how the exchange treats it. A fill is always at the quote's own price.

The fourth row has a condition worth spelling out. If a real order on our side
is at least as good as ours and the arriving order rested anyway, the recorded
book is locked or crossed, the arrival did not trade with the real order, and
nothing can be concluded about ours. That happens outside continuous trading
and in malformed feeds, and the simulator then does nothing.

### 11.3 What is assumed

Each assumption either flatters the result or hurts it, and it is worth knowing
which.

| | Assumption | Effect |
|---|---|---|
| a | **No market impact.** The recorded orders arrive exactly as they did. A real quote changes what others do. And the shares that "would have traded with us first" still trade with the recorded order too, so that liquidity is counted twice | flatters |
| b | **No hidden orders.** Executions against non-displayed orders (`P` messages) do not fill us and do not move the queue. A hidden order at a better price would have been ahead of us | mixed |
| c | **Queue position is exact** for displayed orders (section 11.2) | neither |
| d | **One fixed latency** for new quotes and for cancels. Real latency varies and is worst when the market is busiest, which is when it matters | flatters |
| e | **One change in flight per side.** While a cancel or a new quote is on its way, further wishes for that side wait until it has landed | hurts slightly |
| f | **Quotes are passive.** One that would trade on arrival is refused, never executed as a taker | neither |

Fees are not modelled beyond one number: a rebate (or fee, if negative) per
share filled. It is zero unless given. An exchange's fee schedule is an input,
not something this project knows.

### 11.4 Time

A strategy states what it wants; the simulator turns the difference from what
is resting into a cancel, a new quote or both, and each takes effect one
latency later. Until then a cancelled quote can still be hit, and a new one is
not in the queue. A new quote joins behind whatever is at its price when it
arrives, not when it was decided.

Changes are carried out in the order they would reach the exchange: by time,
and at the same time cancels before new quotes. The second rule is what lets a
strategy move both quotes up together: the new bid may go where the old ask
was, because the old ask has gone by the time it arrives.

The simulator only does anything when a message for the quoted security
arrives, and that is exact, not an approximation. Between two messages of a
security its book does not change. So a quote that was due to arrive between
them joins the same queue, and a markout due between them reads the same mid,
whether it is handled at its own time or at the next message.

The end of the quoting window is the one event known in advance, so no latency
applies to it: quotes are out by then.

### 11.5 Money

Money is kept in integers, like prices everywhere else. The mid of 10.00 and
10.01 is 10.005, which is not a whole price unit, so the mid is kept doubled
(bid + ask), and so is every figure measured against it.

Three figures describe the result, and they are tied by an identity:

```
profit, with the remaining position valued at the last mid
    =  earned at the moment of each fill       sum of shares x (mid - price) for buys,
                                                              (price - mid) for sells
    +  from holding the position afterwards     sum over moves of the mid of position x move
    +  rebates
```

It holds exactly, not approximately. A fill changes the profit by exactly its
distance from the mid, and a move of the mid changes it by exactly the position
times the move, and nothing else changes it. The simulator checks it after
every message in the tests and `mm_sim` refuses to report a clean result if it
fails.

**Markout** is the same measurement as "earned at the fill", taken later: the
distance of the fill price from the mid one second and ten seconds afterwards.
The first figure is what quoting away from the mid earns; the difference
between it and the markout is what it costs to be filled just before the price
moves against the fill. That cost has a name, adverse selection, and it is not
an accident of this simulator. Look at the ways a quote trades in section 11.2:
a bid is filled when a seller has taken everything ahead of it at its price, or
has traded through its price. Both are what a falling market looks like. A
quote is filled disproportionately when it is about to be wrong.

### 11.6 Strategies

A strategy is one function: given the real best bid and offer, the position
and the time, what do you want resting on each side? It is resolved at compile
time, like the feed handler. Three are written:

| Strategy | What it does | What it is for |
|---|---|---|
| `NoQuotes` | nothing | checking that the simulator leaves the replay alone |
| `JoinBest` | joins the real best bid and offer at the back of the queue | the strategy that depends most on the queue model |
| `FixedSpread` | quotes a fixed distance either side of the mid | the baseline to compare others with |

Both stop adding to a position at a limit, and know nothing else about risk.

**Avellaneda-Stoikov** (`avellaneda_stoikov.hpp`) is the textbook step up: it
moves both quotes against the position and widens them with volatility and with
the time left. It is left to be written by hand; its header has the contract,
the questions to be able to answer, and a test suite whose expected prices are
worked out by hand in the comments.

`EwmaVariance` (`volatility.hpp`) supplies the volatility it needs. The mid is
observed at irregular times, so the estimate is the sum of squared moves
divided by the sum of the time they took, both faded with age. Dividing the
sums, and not averaging the ratio move by move, is what stops two changes a
microsecond apart from contributing an enormous value on their own.

### 11.7 How it is tested

1. **The fill model, one rule at a time** (`sim_scenario_test.cpp`): a small
   real book built by hand, a strategy that quotes what the test tells it to,
   and real orders played around the quote. Every row of the table in 11.2,
   every refusal, halts, the quoting window, latency, and the arithmetic of
   every money figure.
2. **Whole markets** (`sim_property_test.cpp`): the feed a matching engine
   published under seeded random flow, checked after every message. The test
   keeps its own copy of the security's orders, knowing nothing about queues.
   When a quote joins, it notes which orders are at that price; from then on
   the simulator's "shares ahead" must equal what is left of exactly those
   orders. That checks who is counted as ahead and who as behind, which is the
   part of the model that cannot be checked by arithmetic.
3. **The money, recounted** from the list of fills alone, and the identity of
   section 11.5 after every message.
4. **The book inside the simulator** against a plain replay of the same bytes.
5. **That the runs reach every case**: every kind of fill and refusal occurs
   across the seeds, or the properties above would be vacuous.
6. **The strategies and the estimator** against figures worked out by hand.
7. **The programs** (`scripts/mm_sim_smoke.sh`): `flow_gen` writes a market,
   `mm_sim` runs over it; a strategy that never quotes makes nothing, the same
   run twice writes the same report, a missing symbol is an error.

None of this says anything about any strategy. The markets in the tests are
generated, and their numbers mean nothing.

## 12. Journal and recovery (`include/obe/journal`)

An engine that lives only in memory forgets every resting order the moment
its process dies. This layer is what lets it be killed at any instant and come
back holding exactly what it held. It is the first of the spec's stretch
ideas.

### 12.1 The idea: record what went in

The matching engine reads no clock and makes no random choice. What it does
is fixed by the requests it is given, their order, and the times they carry
(section 8.1). So its state never has to be saved in order to survive. It is
enough to save the requests. Give them again, in order, to a fresh engine, and
it arrives where the old one was and says, on the way, exactly what the old
one said.

That is why the journal holds requests and nothing else: no trades, no book,
no reports. Recording results as well would give two descriptions of the same
history that could disagree, and nothing to say which to believe.

Two things follow.

1. **The request is written before the engine acts on it** ("write-ahead").
   Had the engine acted first and the process died before the write, the
   outside world could have seen a trade that no replay would ever produce
   again.
2. **Recovery costs as much as the history is long.** Replaying a day of
   requests takes about as long as matching them did. A snapshot
   (section 12.6) is the shortcut: the engine's state at one moment, and the
   journal only from there.

### 12.2 Shape

| File | What it is |
|---|---|
| `records.hpp` | the four records, one per thing an engine can be asked: open an instrument, submit, cancel, replace. Field lists in the style of the feed messages, so the feed codec encodes them |
| `writer.hpp` | `JournalWriter<Out>`: numbers the records, frames and checksums them into a buffer, and hands the buffer to `Out` on `flush()`. `Out` is a file in the programs and a vector in the tests |
| `reader.hpp` | `JournalReader`: reads records back and classifies how the journal ends |
| `replay.hpp` | `replay()`, which gives a journal to an engine; `Journaled<Engine, Writer>`, an engine wrapper that records each request and then passes it on; `state_digest()` |
| `snapshot.hpp` | `capture`, `encode`, `decode`, `plausible`, `restore` |
| `file.hpp` | `JournalFile` (append, `fdatasync`, cut), and `write_file_atomically` for snapshots |
| `recover.hpp` | `recover()`: snapshot if sound, then the journal, then cut a torn tail |

`Journaled` has the engine's own interface, so anything that drives an engine
can drive a journaled one instead, and the engine underneath does not know.
Replay goes through the engine's ordinary operations, so it needs nothing from
an engine beyond the contract of section 8: the hand-written engine can be
journaled and recovered as it is.

### 12.3 The file

```
file header   16 bytes   "OBEJ", version (u16), zero (u16), number of the first record (u64)
record        14 bytes   crc (u32)   CRC-32 of everything after this field
                         size (u16)  bytes in the body
                         seq (u64)   one more than the record before
              size bytes body: a type byte, then that record's fields
```

Integers are big-endian, like every other format here. A submit is 34 bytes
of body, so a request costs 48 bytes on disk.

Why each field is there:

1. **The checksum** lets a reader tell a whole record from one that was being
   written when the machine stopped. A cut-off record is the ordinary way for
   a journal to end, so this is not error handling at the edge of the design;
   it is the centre of it. CRC-32 finds every error confined to 32 consecutive
   bits, which covers the usual damage: a missing tail, a zeroed block.
2. **The sequence number** catches what a checksum cannot: a record that is
   intact but missing, repeated, or from another file. It is also what joins a
   journal to a snapshot and one journal file to the next: the file header
   says which record the file starts with.
3. **The size** makes the format readable by code that does not know every
   record type, and lets the reader check a record's checksum before it
   believes its type.
4. **Fields are stored as given, valid or not.** A side byte that is neither
   `B` nor `S` is a request the engine rejects, and the rejection is part of
   what a replay has to reproduce. The journal does not tidy its input.

### 12.4 How a journal ends

| The reader finds | Called | Meaning | What recovery does |
|---|---|---|---|
| the end of the file, after a whole record | clean end | the writer finished a write | carries on |
| a record it cannot read, and nothing readable after it | torn tail | the machine stopped during a write | cuts the file back to the last whole record and carries on |
| a record it cannot read, and good records after it | corrupt | something damaged the middle of the file | stops and reports; changes nothing |
| an intact record with the wrong number | bad sequence | records are missing, or two files were joined wrongly | stops and reports |
| an intact record of a type it does not know | unknown record | written by a newer version | stops and reports |

The second and third rows cannot be told apart by looking at the bad record:
a damaged length field looks exactly like a record that runs off the end of
the file. So after a failure the reader looks ahead for anything that parses
as a later record, with a matching checksum and a number not yet reached, and
decides by whether it finds one. The number is part of the test: after a
crash a filesystem can leave old blocks showing at the end of a file that was
being extended, and an intact record with a number already passed is that,
not a good record beyond damage.

The difference matters because the two call for opposite actions. A torn tail
holds nothing that was ever complete, so cutting it loses nothing. After
damage in the middle, the records beyond it are good and cannot be applied
(the ones before them are missing), and cutting the file there would destroy
them. That is a decision for a person, with the file intact in front of them.

One case is beyond telling: damage inside the very last record looks like a
torn tail and is treated as one. If that record had been synced and its
result announced, cutting it loses an acknowledged request. Closing that gap
takes a second copy of the journal somewhere else, which is replication and
is not here (section 14).

### 12.5 What "written" means

`write()` returning does not put bytes on a disk. It gives them to the
operating system, which writes them out when it gets round to it.
`fdatasync()` returns only when the device reports them stored.

| What fails | Written, not synced | Written and synced |
|---|---|---|
| the program crashes | survives | survives |
| the operating system crashes, or the power goes | lost | survives |

A sync is slow, on the order of a millisecond on a consumer SSD against about
a microsecond for the write, so a journal that syncs after every request
handles about a thousand requests a second. **Group commit** is the way out:
gather the requests that arrived in one turn of the event loop, write them
together, sync once, and only then let their results out. One sync is shared
by the whole batch. Each request waits a little longer; the cost per request
falls with the size of the batch.

The rule that makes it safe is the last clause: **nothing may be said about a
request that a crash could still erase.** `Journaled` takes a batch size and
has `commit()`; holding the engine's output back until the commit is the
caller's job, because only the caller knows where the output goes.

`fdatasync` is used and not `fsync`: it stores the data and whatever metadata
is needed to read it back (the file's length), and skips the rest (the
modification time), which on some filesystems saves a second write.

All of this assumes the device does what it says. Some drives acknowledge a
flush they have not performed, and some virtual and network filesystems do
not pass the flush on. `journal_bench` measures the device it is pointed at:
a sync that takes microseconds is a sync that did not happen.

### 12.6 Snapshots

The test of a snapshot is that an engine restored from it cannot be told from
the original by anything that happens afterwards. That takes more than the
book:

| In the snapshot | Why |
|---|---|
| the open instruments and their symbols | |
| every resting order: id, owner, token, side, price, open shares | |
| the orders of each level listed oldest first | time priority is not a field of an order; it is the order's place in a queue |
| the last order id and the last match number | they are in no order and no level, and without them the restored engine would give its next order an id that is already resting |
| the running totals | so they carry on instead of restarting |
| the number of the first journal record it does not include | where to pick the journal up |

Level totals, best prices and the index from id to order are left out: they
can be worked out, and what can be worked out cannot be wrong.

A snapshot file is checked whole (length against the counts inside it, then a
CRC over all of it) and then the state is checked for sense (`plausible`:
every order belongs to a listed instrument, no id twice or beyond the counter,
no book crossed) before any of it reaches an engine. Unlike a journal, half a
snapshot is worth nothing.

It is written with `write_file_atomically`: to a temporary name, synced,
renamed over the real name, and the directory synced. A crash at any point
leaves the old snapshot or the new one, never a mixture.

Two rules hold the pair together.

1. **The journal is made durable before the snapshot is written.** A snapshot
   says "I include everything before record N". If it reached the disk and the
   journal's records before N did not, the two would describe different
   histories. `recover()` notices a journal that ends before its snapshot and
   refuses to guess.
2. **The journal is the record; a snapshot is a convenience.** A snapshot that
   fails any check is ignored and recovery replays the journal from the
   start.

Loading state directly needs more from an engine than the contract of
section 8. Those five functions are the `Restorable` concept in
`engine/concepts.hpp`. The reference engine has them. They are optional: an
engine without them is still journaled and recovered by replay.

### 12.7 `engine_journal` and the crash test

`engine_journal run` drives an engine with seeded flow through a journal. If
the journal is already there it recovers first and carries on from the next
request. `check` replays without changing anything and `dump` prints the
records.

`scripts/journal_crash_test.sh` holds it to one standard. A run is made that
is never killed. Another is killed over and over and restarted with the same
options until it finishes. Its journal must then be the same file, byte for
byte. Anything recovery gets wrong (a request lost, applied twice, applied to
the wrong state) changes what the engine does next, and with it every byte
written afterwards.

Killing a process from outside is not enough to test this. `kill -9` lands
between two system calls, so the file always ends where some write ended, and
the torn record that recovery exists for never appears. So the program tears
its own write: `--kill-at-flush K --kill-keep PCT` writes only part of the
K-th batch and then sends itself `SIGKILL`.

The load generator in that program stands for the outside world, which does
not crash when the engine does. To put it back where it was, a restarted run
feeds it its own requests again from the first, against a second engine that
is then thrown away. That costs as much as a full replay and is a cost of the
demonstration, not of recovery; the two are timed and printed separately.

### 12.8 How it is tested

1. **The claim itself** (`tests/journal/recovery_test.cpp`, typed over the
   engines). An engine rebuilt from a journal holds what the original held and
   said, during the replay, every report and every market-data message the
   original said. Then both are given the same further requests and must
   answer identically, which is what shows the ids, the match numbers and the
   queue positions came back right.
2. **Every way of stopping short.** The journal is cut after each record, and
   at chosen bytes around and inside every record; each time the engine must
   be the one that existed after the last whole record. A journal cut, then
   carried on, must equal one that was never cut.
3. **Group commit** loses the unfinished batch and nothing else, and how often
   the writer flushes does not change a byte of the file.
4. **Snapshots** (`snapshot_test.cpp`): restore, then compare as in 1; a
   snapshot plus the journal after it; each thing a snapshot could forget
   (queue order, the counters) has a test that fails without it; every
   truncation and every single-bit error of a snapshot file is refused.
5. **The files** (`journal_file_test.cpp`): recovery from a real directory,
   including a torn tail cut and carried on, a damaged snapshot passed over,
   and a journal that ends before its snapshot.
6. **The reader against damage** (`journal_format_test.cpp`): truncation at
   every byte, every single-bit error, trailing zeros, and the line between a
   torn tail and corruption.
7. **`fuzz/journal_fuzz.cpp`**: on arbitrary bytes, the part of a journal the
   reader vouches for is itself a clean journal; on journals built from the
   input, any cut gives a prefix and any changed byte is noticed and never
   applied.
8. **The crash test** above, on the real program.

What none of this shows is that a sync reached the disk. The tests count the
calls to `fdatasync` and check they are made in the right places; whether the
bytes survive a power cut can only be shown by cutting the power, or with a
block device that can be told to drop unsynced writes (dm-flakey and the
like). Take the calls out and every test here still passes. That is a known
gap, and the reason section 12.5 says what the code relies on the device for.

## 13. Decisions that are open to change

These were made along the way to get each phase standing. Each is cheap to
revisit.

1. **`AddOrder` covers both `A` and `F`.** One struct with an `attributed`
   flag, one `on_add` callback. The alternative is two structs and two
   callbacks, which is more faithful to the wire and makes every handler
   implement both.
2. **`OrderRecord` does not store the locate.** The message header supplies it.
   A message carrying the wrong locate is caught as a `level_mismatch`, not by
   comparing locates. Storing it would cost two bytes that currently fall in
   padding, and is a candidate for the struct-layout experiment.
3. **Strict parsing.** Unknown types stop the parse. A forward-compatible
   "skip unknown types of plausible length" mode would be easy to add if a
   newer sample file ever needs it.
4. **Open-share tracking on every message.** `BookManager` keeps a running
   total of resting shares so `audit()` can compare it with the levels. It is
   one add or subtract per message. If it shows up in a profile it can move
   behind a template flag.
5. **`itch_synth` is not a market model.** It picks random resting orders to
   execute. It is for fixtures that need every message type and for
   smoke-testing the harness. `flow_gen` (section 8.5) is the generator to use
   when the shape of the book matters.
6. **Replace quantity means open quantity.** `replace(id, qty, price)` sets the
   shares the order should have open afterwards. Exchange protocols such as
   OUCH define it as the order's total size including what has already traded.
   The gateway (phase 7) can translate; the engine's version needs no memory of
   past fills.
7. **The engine's sinks are held by reference.** They outlive the engine and
   must not call back into it. Holding them by value, as `BookManager` holds
   its listener, would make the generator's feedback loop awkward: the flow
   generator is both the source of requests and a report sink.
8. **The parser thread decodes.** What crosses the first queue is a decoded
   `BookEvent`, not a pointer to the raw message. Passing pointers would make
   the first stage nearly idle; whether that would be faster overall is a
   measurement `pipeline_bench` can make if a second encoding is added.
9. **One item per queue operation.** A stage pushes and pops single items.
   Moving several per operation would cut the number of times the two sides
   touch each other's cache lines, at the cost of holding items back. It is
   the first thing to try if the pipeline loses to the single thread.
10. **Spinning backs off to yielding.** `util::Backoff` spins 256 times and
    then yields the time slice. On a machine with a core per thread the yield
    is never reached; on one without, it is what keeps the run from stalling.
    A latency-critical deployment would spin without limit and reserve the
    cores.
11. **The simulator lives in this repository.** The spec leaves open whether
    phase 8 belongs here or in a project of its own. It is here, in three
    directories that nothing else depends on (`include/obe/sim/`,
    `apps/mm_sim.cpp`, `tests/sim/`), so that moving it out is a matter of
    moving those directories and their entries in the build files.
12. **Queue position by order reference.** The spec's outline tracks the
    shares ahead of a quote and assumes where cancels fall. The feed names
    every order, so the simulator tracks which orders are ahead instead, and
    needs no assumption for displayed orders (section 11.2).
13. **A strategy states what it wants; it does not send orders.** The
    simulator works out the cancels and new quotes, and keeps a quote whose
    price has not changed where it is in the queue. The alternative, a
    strategy that sends and cancels orders itself, is closer to a real system
    and makes every strategy repeat the same bookkeeping.
14. **A partly filled quote is not topped up.** While its price is still the
    one wanted it stays as it is, with what is left of it. Adding size would
    mean a new order at the back of the queue.
15. **The journal records requests, not results.** It is the smaller of the
    two and cannot disagree with itself (section 12.1). The price is that the
    journal is only as good as the engine is deterministic: a change to the
    matching rules changes what an old journal replays to, so a journal
    belongs to the version of the engine that wrote it.
16. **Journaling is a wrapper, not part of the engine.** `Journaled` has the
    engine's interface and the engine does not know it is there. Any engine
    can be journaled, and the engine's hot path carries no branch for it.
17. **CRC-32, computed with a table.** The same polynomial as zlib and
    Ethernet, a byte at a time. CRC-32C with the processor's own instruction
    is several times faster and is the obvious experiment if the "journal,
    discarded" row of `journal_bench` shows the checksum mattering.
18. **A torn tail is cut without asking; damage in the middle never is.**
    Section 12.4 gives the reason for each half.
19. **Restoring from a snapshot is an optional capability.** The five
    functions are a separate concept and not part of the engine contract, so
    that an engine is not required to expose its insides in order to be
    usable.

## 14. Known limits

1. Linux only: `mmap`, `epoll`, `perf_event_open`.
2. GCC and Clang only: `__builtin_bswap*` and, in the benchmarks, `rdtsc`.
3. One venue, one feed. A gap in the UDP feed is detected and counted, never
   recovered: there is no re-request channel and no snapshot.
4. IPv4 only, and host names are not resolved.
5. Order entry has no session layer: no login, no heartbeat, no resumption
   after a reconnect, no authentication of any kind. It is for loopback and a
   lab network, not for anything exposed.
6. The exchange is one thread. Its throughput is that of one core, and a slow
   stretch anywhere in the loop delays every client.
7. The simulator quotes one security per run, with one quote per side, and
   knows nothing of hidden orders, fees beyond a flat rebate, or the reaction
   of other participants to its quotes (section 11.3). Its output compares
   strategies; it does not predict what one would earn.
8. The journal is not connected to the exchange. `exchange_server` runs its
   engine without one. Doing it properly needs two things that are not here:
   the gateway has to hold every response and every market-data packet until
   the batch containing its request is durable, and clients need a session
   layer (limit 5) to learn, after reconnecting, which of their requests
   survived.
9. One journal file that grows without end. The format lets a file start at
   any record and the tests replay a journal kept in several files, but
   nothing starts a new file or deletes the ones a snapshot has made
   unnecessary.
10. One disk on one machine. A journal protects against the process and the
    machine stopping, not against the disk dying: that takes a second copy
    somewhere else, written before a request is acknowledged.
11. Snapshots work for the reference engine only, until the hand-written
    engine is given the five `Restorable` functions.
