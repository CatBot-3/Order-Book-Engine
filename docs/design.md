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

`NaiveBook` (`tests/support/naive_book.hpp`) is the oracle: a flat map of
orders that recomputes the best bid and offer by scanning every order after
every message. It is far too slow for real data and very hard to get wrong.

All of this runs under AddressSanitizer and UndefinedBehaviorSanitizer in the
`debug` preset.

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
| Matching loop | phase 5 | not started |
| Lock-free queue | phase 6 | not started |

Each "to write" header holds the interface, the questions to settle and where
the measurements that settle them come from. Their tests are built into a
second executable, `obe_hand_written_tests`, from the same sources as the
reference tests, and carry the `ctest` label `needs-your-code`:

```sh
ctest --preset debug -LE needs-your-code     # everything that is finished
ctest --preset debug -L needs-your-code      # the hand-written parts
```

Until a container is written its tests fail with a message naming the missing
function, and `--impl <name>` in the apps exits with status 4. Record the
decisions you make for each one in sections 6.3 onwards.

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

## 8. Decisions that are open to change

These were made to get phases 1 to 3 standing. Each is cheap to revisit.

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
5. **The synthetic generator is not a market model.** It picks random resting
   orders to execute. It is for fixtures and for smoke-testing the harness, and
   numbers measured on it mean nothing about real data. Phase 5 replaces it.

## 9. Known limits

1. Linux only: `mmap`, and later `epoll` and `perf`.
2. GCC and Clang only: `__builtin_bswap*` and, in the benchmark, `rdtsc`.
3. One venue, one feed, no gap recovery (a file has no gaps).
