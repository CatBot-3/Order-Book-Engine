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
| Engine scenario | `tests/engine/engine_scenario_test.cpp`, `instructions_test.cpp` | every order type, every instruction and every edge of the engine contract: reports, market data and the book left behind |
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
| Varint | `tests/util/varint_test.cpp` | every size boundary, truncation at every length, overlong and overflowing encodings refused |
| Tick codec | `tests/store/tick_codec_test.cpp` | lossless for every tick, likely or not; nothing survives a reset; no read outside the buffer on arbitrary or damaged bytes |
| Tick store | `tests/store/tick_file_test.cpp`, `fuzz/tick_store_fuzz.cpp`, `scripts/tick_store_smoke.sh` | ranges of time against a filter over the plain list; a store never finished, cut at every byte, with every single-bit error, with an index that lies; the real program against a replay of the feed |
| Compression | `tests/store/compression_test.cpp` | the hand-written codec's size on generated ticks, against a first target |

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
| Matching engine (pooled, intrusive queues), including the instructions of section 8.6 | `include/obe/engine/matching_engine.hpp` | **to write** |
| Mutex queue, spin channel, seqlock, pipeline | `include/obe/util/mutex_queue.hpp`, `spin_channel.hpp`, `seqlock.hpp`, `include/obe/pipeline/` | written |
| Lock-free ring | `include/obe/util/spsc_ring.hpp` | **to write** |
| Gateway, market-data publisher and receiver, load generator | `include/obe/net/`, `apps/` | written |
| Market-making simulator, fill model, simple strategies, volatility estimate | `include/obe/sim/` | written |
| Avellaneda-Stoikov strategy | `include/obe/sim/avellaneda_stoikov.hpp` | **to write** |
| Journal, replay, snapshot, recovery from files | `include/obe/journal/` | written |
| Snapshot support for the hand-written engine | the five `Restorable` functions, in `include/obe/engine/matching_engine.hpp` | optional: see below |
| Tick store: varints, the file, the reference codec, the profile | `include/obe/util/varint.hpp`, `include/obe/store/` | written |
| Compressing tick codec | `include/obe/store/delta_codec.hpp` | **to write** |

Each "to write" header holds the interface, the questions to settle and where
the measurements that settle them come from. Their tests are built into
second executables (`obe_hand_written_tests` for the containers,
`obe_hand_written_engine_tests` for the engine,
`obe_hand_written_concurrency_tests` for the ring) from the same sources as the
reference tests, and carry the `ctest` label `needs-your-code`. The strategy
has an executable of its own, `obe_hand_written_sim_tests`, under the same
label, and the tick codec has `obe_hand_written_store_tests`:

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

### 6.9 Compressing tick codec

*(yours)*

Not on the spec's list either, and left to write for the same reason as the
strategy: it is the one part of the tick store that is an idea and not
plumbing. The file, the index, the recovery and the tools are done and tested
around a codec that does not compress (section 13).

The order of work the header suggests is the reverse of the obvious one. Run
`tick_store profile` on a real file first, and write down what the table
says before designing anything: a codec is a bet on that table, and one
designed from a guess about markets is usually a bet on the wrong thing.
Then the simplest design that is correct, measured, and after that one
experiment at a time in [`optimization-log.md`](optimization-log.md), each
with bytes per tick and ticks per second before and after, and the same file
through `zstd` beside them.

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
numbers itself, counting up. Together those make a run a pure
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
   orders, for every order that asks for no self-match prevention.
5. **A market order never rests.** With no price there is no level to put it
   on. What it cannot fill is cancelled, with a reason that says the book ran
   out.
6. **A refused request changes nothing and uses no id.** The checks run in a
   fixed order and the first failure is the one reported. The order of the
   checks is part of the contract only because two engines have to agree on
   it.
7. **Self-trading is allowed unless an order asks otherwise.** An owner's
   order can trade with another of their own. An order can carry an
   instruction that prevents it (section 8.6).

Left out on purpose: auctions, halts, price bands, lot-size rules, stop and
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

1. **Scenarios** state, for each order type, each instruction and each edge,
   the exact reports, the exact market data and the exact book afterwards.
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
   replace, sweeps, replaces that trade, new slices of icebergs, and
   self-match prevention stopping orders on both sides. Without it the
   properties could pass on flow that never visits the hard cases.
6. **The differential test** holds the hand-written engine to the reference's
   output: market data byte for byte, reports one by one.

### 8.5 The order-flow generator (`include/obe/gen/order_flow.hpp`)

`OrderFlow` produces requests, not messages. Each symbol has a mid price that
takes a random walk; most orders are placed a few ticks behind it, a minority
reach across it, and cancels and replaces pick a random resting order. The
number of resting orders hovers around a target. A small share of requests is
wrong on purpose, to exercise the rejects. Three rates, all zero by default,
give orders the instructions of section 8.6; with all three at zero a seed
produces exactly the requests it always did.

A seed means the same requests on every compiler, and that had to be made
true. Two random draws written as two arguments of one call are made in an
order the language leaves to the compiler, and GCC and Clang choose
differently: for a while the same seed gave one flow under GCC and another
under Clang, and nothing noticed, because no test compared a flow with
anything but itself. The draws are now in statements of their own, and a
test pins the hash of a flow to a number
(`tests/gen/order_flow_test.cpp`). A tape recorded by one build can be
compared with one recorded by another only since then.

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

### 8.6 Instructions: post-only, icebergs, self-match prevention

Three instructions can ride on an order (`NewOrder::post_only`, `display`,
`self_match`). They are the second of the spec's stretch ideas. The exact
rules are under "Instructions" in `concepts.hpp`; what follows is why each is
shaped as it is.

**Post-only** means "rest, or do nothing". An order that would trade on
arrival is cancelled. It exists because of how venues charge: a resting order
is usually paid a rebate and an arriving one pays a fee, so a participant
quoting for the rebate wants a guarantee that an order never crosses because
the market moved while it was on the wire.

1. It is Accepted and then Cancelled, not Rejected. It was a valid order that
   the book had no room for, like a fill-or-kill that is killed, and it uses
   an id. Rejected is kept for requests that are wrong in themselves.
2. A replace that would make a post-only order trade is refused and the order
   stays where it was. The alternative, cancelling the order, would turn an
   attempt to improve a quote into losing it.
3. "Would trade" is about price only. It is decided before any trade, so
   self-match prevention, which is a rule about trades, has no say in it.

**An iceberg** shows only part of itself. A large order displayed in full
tells the market that somebody has a lot to do, and the price moves away
before it is done.

1. **The market sees displayed shares only**, in the feed and in the level
   totals. The owner's reports count everything.
2. **A new slice goes to the back of the queue.** When the displayed shares
   are used up, the next slice has no claim on the place the old one earned.
   If it kept the place, an iceberg would be a way to hold the front of a
   queue with an unlimited amount while showing a little.
3. **A new slice has a new name in the market data.** It takes the next id as
   its order reference, as Nasdaq does, so that an observer cannot tell a new
   slice from a new order. The order's own id, which the owner uses and every
   report carries, does not change. So an order now has two names, and ids are
   no longer consecutive: this is the one place the instructions changed
   something for orders that do not use them.
4. **Hidden shares can be traded with.** An order larger than what a level
   shows meets the same iceberg again, a slice at a time, each one a separate
   trade behind everything else at that price. Fill-or-kill counts them.
5. **Shrinking an iceberg takes from the hidden part first**, and the market
   hears nothing until the displayed part shrinks.

**Self-match prevention** stops an order trading with another order of the
same owner. A firm running several strategies does not want them paying fees
to trade with each other, and in most markets a trade with oneself is a
regulatory problem: it prints volume that no change of ownership stands
behind.

1. **It acts at one moment**: when the order the incoming one would trade with
   next belongs to the same owner. An order of the same owner further down the
   book, never reached, changes nothing.
2. **Three modes, and the incoming order chooses**: cancel the incoming order,
   cancel the resting one and carry on, or cancel both. There is no "right"
   one; which order a firm would sooner lose depends on which strategy sent
   it.
3. **The instruction stays with a resting order.** It applies again if a
   replace sends that order across the book, which is otherwise a way round
   the rule.
4. **Fill-or-kill has to know in advance**, and this is where the three
   instructions meet. Whether a fill-or-kill order can be filled now depends
   on whose orders are in a level and in what order: hidden shares of an
   iceberg that sits ahead of the owner's own order come back *behind* that
   order, and an incoming order that stops at its own order never reaches
   them. The reference engine answers by reasoning about each level. The test
   oracle answers by doing the trades with its output switched off and then
   undoing them. They are checked against each other on every fill-or-kill in
   the random flows, which is the only reason to trust the reasoning.

`instructions_test.cpp` has one hand-worked scenario per rule, run on the
reference engine and on the oracle. The flows used by the property tests now
include three in which a quarter of the resting orders are icebergs, an eighth
are post-only, and two in five ask for prevention, with three owners so that
orders keep meeting their owner's others. Every property is checked on them,
and so are the journal and snapshot suites: an engine restored with icebergs
in the middle of their slices must carry on exactly as the original.

Not done: stop orders, pegged orders, minimum quantity, good-till-date,
decrementing self-match modes, and prevention across related owners (a real
venue matches on a firm or group identifier, not on one owner id).

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
session. An Enter Order carries the three instructions of section 8.6 as a
display size and two bytes; a byte that is not one of the defined values is
refused by the gateway, like an unknown kind or time in force, because the
engine has no way to represent it.

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

Integers are big-endian, like every other format here. A submit is 40 bytes
of body, so a request costs 54 bytes on disk.

The version is 2: the submit record grew when orders gained instructions
(section 8.6). A version 1 file is refused by its header, not converted. A
journal is a record of what one version of the engine was asked, and it
replays to the same state only on that version (section 14, item 15).

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
is not here (section 15).

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
| what only some orders have: an iceberg's hidden shares, its display size and the reference the market currently knows it by; post-only and self-match instructions | a restored iceberg must show its next slice at the right moment and be cancelled under the name the market knows; the instructions still bind the order when a replace moves it |
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

## 13. Tick store (`include/obe/store`)

The book publishes its best bid and offer every time it changes, and until
now nothing kept what it published. This layer keeps it: every update of a
day in one file, compressed, and answerable for "what happened between 10:00
and 10:05" without reading the day. It is the third of the spec's stretch
ideas.

### 13.1 Shape

| File | What it is |
|---|---|
| `util/varint.hpp` | variable-length integers and zigzag, the two building blocks of a compact format |
| `store/tick_codec.hpp` | what a tick is (`book::BboUpdate`), the contract of a codec, and `RawCodec`: every field at full width, 34 bytes a tick |
| `store/delta_codec.hpp` | `DeltaCodec`, the compressing one. **Written by hand** (section 6.9) |
| `store/tick_file.hpp` | the file: `TickWriter`, `TickRecorder` (a book listener), `TickReader` |
| `store/tick_profile.hpp` | `TickProfile`: what consecutive ticks differ by, measured |
| `store/codecs.hpp` | the codecs by name and by the number a file carries |
| `gen/tick_gen.hpp` | generated ticks for the tests and the benchmark |
| `apps/tick_store.cpp` | `record`, `info`, `query`, `verify`, `profile` |
| `bench/tick_bench.cpp` | bytes per tick, write and read cost, and query cost by block size |

The split that matters is between the codec and the file. The codec knows how
one tick after another becomes bytes and nothing about files. The file knows
about blocks, checksums and the index, and nothing about how a tick is
encoded. The reference codec makes the file testable before the compressing
one exists, and every test of the file is run again on the compressing one
when it does.

### 13.2 What a codec promises

Three functions (`tick_codec.hpp`):

| Function | What it does |
|---|---|
| `reset()` | forgets everything |
| `encode(tick, out)` | writes one tick and returns its length, at most `kMaxTickSize` |
| `decode(p, end, tick)` | reads one and returns its length, or 0 if no whole tick is there. It never reads at or past `end`, whatever the bytes are |

A codec may keep state between calls, and compressing ones live on it: the
last tick they saw is what the next one is written as a difference from. Two
rules keep that safe.

1. **The encoder and the decoder keep the same state.** After writing ticks
   1 to k from a reset, and after reading them back from a reset, both are
   ready for tick k+1 in the same way. Everything else follows from this.
2. **Lossless for every tick, not for likely ones.** Any time in any order,
   any price, any size up to 2^64 - 1 comes back exactly. A codec is free to
   be bad at ticks no market produces. It is not free to be wrong about them:
   a store that is right "for realistic data" is right until the day the data
   is unusual, which is the day someone needs it.

The tests hold a codec to both with streams no feed would produce
(`hostile_ticks` in `tests/support/tick_streams.hpp`) as well as with ones
shaped like a market.

### 13.3 The file

```
file header   16 bytes   "OBET", version (u16), codec (u16), ticks per block (u32), zero (u32)
block         32 bytes   "BLK1", crc (u32), ticks (u32), payload bytes (u32),
                         earliest timestamp (u64), latest timestamp (u64)
              payload    the block's ticks, encoded from a reset
... more blocks ...
index         24 bytes   "IDX1", crc (u32), blocks (u32), symbols (u32), ticks (u64)
              32 each    per block: offset, ticks, payload bytes, earliest, latest
              10 each    per symbol: locate (u16), name (8 characters)
trailer       12 bytes   offset of the index (u64), "OBEX"
```

Integers are big-endian, and each checksum is a CRC-32 of everything in its
section after the checksum field, as in the journal.

**Why blocks.** A compressing codec writes each tick against the ones before
it, so reading tick fifty million would mean decoding the forty-nine million
before. Cutting the stream into blocks, with the codec reset at the start of
each, bounds that: any tick is at most one block's decoding away. The price is
that the first ticks of every block have nothing to be a difference from.
Bigger blocks compress a little better and seek a little worse. The size is a
parameter (`--block`, 4096 ticks by default) so that the trade can be
measured, and `tick_bench` measures it at three sizes.

**Why the index is at the end.** It lists each block's place and the range of
time it covers, which is what turns a query for a range into a seek. None of
that is known until the block is written, and a recorder does not know how
many blocks there will be. So the index is written once, last, and the
trailer says where it starts.

**Why the time range is in each block's own header as well.** So that the
blocks describe themselves without the index (next section).

### 13.4 A file without an index is still a store

A recorder that is killed never writes its index. Every block carries its own
length and checksum, so a reader can start at the file header and walk: read a
block header, check the block against its checksum, step over it, repeat,
stopping at the first block that is not whole. The index is rebuilt from what
it found. Such a store opens as *recovered*, and what follows the last whole
block is ignored: it was being written when the recorder stopped.

This is the journal's torn tail again (section 12.4), and for the same
reason: a file that is appended to must expect to be cut off mid-append, and
a format that cannot say where its good part ends has to be thrown away
whole. Only the directory of names is lost, since it lives in the index.

A block is handed to the writer's destination whole, in one call, when it is
full. So a writer that stops loses the block it was filling, by default up to
4095 ticks, and whatever its destination had not yet passed on: `tick_store`
writes through an ordinary buffered file and syncs nothing, because nothing
waits on a tick store reaching the disk.

### 13.5 An index is checked, not trusted

An index with the right checksum holds the bytes that were written. That is
not the same as holding the truth. If its entry for a block gave a time range
narrower than the block's ticks, queries would skip a block that holds part
of their answer and report nothing wrong. So the reader believes an index
only if it is whole, its checksum matches, **and** it describes exactly the
blocks that lie between the file header and itself: each one where the last
ended, each block's own header saying what the index says, and nothing left
over. Otherwise the index is set aside and the blocks are found by walking.

The check reads every block's 32-byte header when the store is opened, and
no payload. On a mapped file that is one page touched per block.

It cannot catch a block whose own header misstates its time range, since
there is then nothing to compare with but the ticks themselves. That takes a
writer with a bug, not a damaged disk, and `tick_store verify` is the check
for it: it decodes everything.

### 13.6 Queries

`scan(from, to, visitor)` hands over every tick with `from <= time < to`, in
the order they were stored, and stops early if the visitor says so. Blocks
whose range cannot hold such a tick are not read; the result says how many
were read and how many were ruled out.

1. **The range is half-open**, so that consecutive ranges neither overlap nor
   leave a gap. One consequence needed an exception: no end time could then
   include a tick stamped with the largest representable time. `kEndOfTime`
   as the end therefore means "no end".
2. **Nothing requires time to rise.** A feed's time does, but the store does
   not depend on it: each block records its earliest and latest, and a query
   looks at every block that could matter. On a store in time order that is
   one or two blocks; on a shuffled one it is most of them, and still right.
3. **All securities share one stream.** That makes a range of time cheap and
   "one stock for the whole day" a scan of every block, filtered. The
   alternative is a stream per security: the reverse trade, better
   compression, and thousands of open blocks while recording (section 14,
   item 24).

### 13.7 Damage

A block is checked against its checksum before any of its ticks is handed
over, so a visitor never sees a tick from a block that was damaged after it
was written. The scan stops there and says which block. Blocks before it have
been delivered; blocks after it are untouched and can be reached by a query
for a later range, because each starts from a reset and depends on nothing
before it.

Where damage is found depends on where it is:

| Damaged | Found |
|---|---|
| file header | when opening: not a store, or another codec's |
| index or trailer | when opening: the index is set aside, the store opens as recovered with every tick |
| a block's header | when opening: it no longer matches the index, so the blocks are walked, and the walk stops at it |
| a block's ticks or its checksum | when that block is read |

The last row is deliberate. Checking every block's checksum on opening would
find the damage sooner and make opening a day's file cost a read of the whole
file, which is what the index exists to avoid. `tick_store verify` is the
full check, for when it is wanted.

The test behind this section changes one bit at a time across a whole file
and requires, each time, that no tick comes back that was not written, and
that when ticks are missing either the way the store opened or the way the
scan ended says so.

### 13.8 Varints, and one encoding per number

`util/varint.hpp` has the usual LEB128 varint (seven bits a byte, the top bit
meaning "more follows") and zigzag (which folds small negative numbers onto
small positive ones). One choice in it is worth stating. The decoder accepts
only the **shortest** encoding of a number: `0x80 0x00`, two bytes that would
decode to zero, is refused.

A decoder that took it would let two different files mean the same thing.
Then "decode and encode back gives the same bytes" stops being true, and that
property is the cheapest strong test a format can have: the fuzzer checks it
on every input. It also closes a small door: padding is where a hostile file
hides length.

### 13.9 The profile, and why the generated ticks are not a market

A compressing codec is a bet on what its input looks like. `tick_store
profile` measures the things such a bet is about, on a real file: the gaps
between timestamps, how soon a security ticks again, which fields of a quote
change together, and by how much. The hand-written codec is meant to be
designed from that table (section 6.9).

The tests and the benchmark need ticks without a file, and
`gen/tick_gen.hpp` makes them: time in steps of up to 50 microseconds,
securities picked evenly, one field changing at a time by a cent or by round
lots. That is the shape of a market with none of its substance. A real feed
is burstier, a few of its securities do most of the ticking, and its sizes
are not all round lots. So the target in `tests/store/compression_test.cpp`
(10 bytes a tick, against 34) says a codec does what a codec should on input
of that shape, and no more. The number for a real day comes from `tick_store
record` on a real file, and has not been taken.

### 13.10 How it is tested

1. **The codec contract** (`tests/store/tick_codec_test.cpp`, typed over the
   codecs): every stream comes back; every field at its limits; time standing
   still and running backwards; after a reset nothing of the past is left, on
   the writing side and the reading side; a run cut into blocks anywhere; a
   tick cut short is not a tick; arbitrary and damaged bytes are read without
   leaving the buffer, which AddressSanitizer enforces because every buffer is
   exactly as large as its data.
2. **The file** (`tick_file_test.cpp`, typed the same way): what the writer
   lays out, block by block; random ranges of time against a filter over the
   plain list; the two ends of a range; that the index spares a query the
   blocks it cannot need; a store never finished; a store cut at every byte;
   every single-bit error; an index that checks out and lies, nine ways; a
   block with a correct checksum over the wrong contents; another codec's
   store; and a recorder on a real book against a plain list of what the book
   published.
3. **The profile** (`tick_profile_test.cpp`): seven ticks chosen so that each
   lands in a different cell, with every table worked out by hand.
4. **The varint** (`tests/util/varint_test.cpp`): each size boundary, the
   largest value, truncation at every length, overlong and overflowing
   encodings refused, zigzag at both ends of the range.
5. **`fuzz/tick_store_fuzz.cpp`**: on arbitrary bytes, a store that opens
   describes itself consistently and a scan hands over only what it claims;
   on stores built from the input, what was written comes back, a cut gives
   whole blocks, and a changed byte never produces a tick that was not
   written.
6. **`scripts/tick_store_smoke.sh`**, on the real program: a generated feed
   recorded and checked against a fresh replay of the feed by the same
   per-security hash that compares two books (section 3.4); ranges compared
   with a filter over the full dump; then the file cut short, and a byte of
   one block changed.

The typed suites run on `RawCodec` in the passing build and on `DeltaCodec`
under the `needs-your-code` label, where `compression_test.cpp` adds the size
target.

What none of this measures is a real day: how many ticks one has, what the
profile of a real feed looks like, and how small a real store is.

## 14. Decisions that are open to change

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
20. **A killed post-only order is Accepted and then Cancelled.** Rejecting it
    would be as defensible, and is what some venues do. Cancelling keeps
    "rejected" meaning "the request was wrong in itself", and treats the
    order like a fill-or-kill that found the book unsuitable.
21. **An iceberg's slices take their references from the order-id counter.**
    One counter, so a reference can never collide with an order id, at the
    price of ids that skip. A separate counter for references would keep ids
    consecutive and need the market data and the owners' reports to use
    different number spaces.
22. **Self-match prevention compares owner ids.** One owner, one identity. A
    venue would compare a firm or group identifier carried on the order, so
    that two traders of one firm are prevented from trading with each other.
    That is a field and a comparison away, and waits for the gateway to have
    a notion of who a connection belongs to.
23. **An order that trades through icebergs rests under its own, older id.**
    Its reference in the market data is then lower than the references the
    new slices took meanwhile, so references are unique and not always
    increasing. Giving every resting order a fresh reference at the moment it
    rests would restore the order, and add a second name to every order for
    the sake of a property nothing here relies on.
24. **The tick store is one stream in time order.** A range of time is a
    seek; one security for a whole day is a scan of every block. A stream per
    security would reverse that and compress better, since each stream's
    ticks are far more alike than the mixture is, at the cost of a block in
    memory for every security while recording and a merge to read anything
    in time order. It is the first thing to revisit if the queries turn out
    to be per security.
25. **A tick is a best-bid-and-offer update.** Depth, trades and the orders
    themselves are not stored. Storing the feed's messages would keep
    everything and need a book replay to answer "what was the quote at
    10:00"; storing the quote answers that directly and cannot answer
    anything about depth. The format has room for a second kind of record
    only through its version number.
26. **A block is a number of ticks, not a number of bytes.** Counting ticks
    makes what a query costs to decode predictable and the writer's buffer a
    fixed size. Counting bytes would make blocks the size of disk pages and
    their reads uniform, which matters more once the store is read from a
    disk and not a mapping.
27. **A reader takes the whole file as one span.** It is mapped, and the
    operating system pages in what a query touches. There is no reader over
    a stream or a socket, and on a 32-bit address space a large day would not
    fit.
28. **An index that does not match its blocks is set aside, not repaired.**
    The store then opens as recovered and nothing rewrites the file. A tool
    that rebuilt the index in place would be a few lines; it would also be
    the only code here that modifies a store after it was written.

## 15. Known limits

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
12. The order types stop at limit and market, three times in force, and the
    three instructions of section 8.6. No stop orders, no pegged orders, no
    minimum quantity, no auctions. The gateway's Accepted message does not
    echo an order's instructions back.
13. A tick store is written once. Nothing appends to a finished store, joins
    two, or splits one, and a query for one security reads every block in its
    range of time (section 14, item 24).
14. Until the hand-written codec exists a store is 34 bytes a tick, and the
    only compression figure is the target on generated ticks. Nothing has
    been recorded from a real day: not the number of ticks, not their
    profile, not a file size.
15. The tick store is not connected to the live programs. `tick_store record`
    replays a file. `md_listen` could record what it receives through the
    same `TickRecorder`, and does not.
