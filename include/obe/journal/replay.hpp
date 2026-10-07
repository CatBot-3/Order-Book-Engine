#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "obe/book/types.hpp"
#include "obe/engine/concepts.hpp"
#include "obe/engine/output_hash.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/codec.hpp"
#include "obe/feed/messages.hpp"
#include "obe/journal/reader.hpp"
#include "obe/journal/records.hpp"
#include "obe/types.hpp"

// Recording an engine's requests, and running them again.
//
// Journaled wraps an engine: every request is appended to a journal and then
// passed on. replay() is the other direction: it reads a journal and gives
// each request to an engine. Because the engine is deterministic, an engine
// that has been through replay() is in the state the recorded one was in
// after the last record, and from then on behaves exactly as it would have.
// The tests hold it to that: a recovered engine and the original are given
// the same further requests and must say the same things.
//
// Replay goes through the engine's ordinary operations, so it needs nothing
// from the engine but the contract in obe/engine/concepts.hpp. Any engine can
// be recovered this way, the hand-written one included.
//
// The engine's output during a replay is the output it produced the first
// time. Whoever recovers decides what to do with it: usually nothing (give
// the engine sinks that discard), sometimes count it, to know what sequence
// number the market data had reached.

namespace obe::journal {

// Gives one record to an engine.
// Precondition: the record came from JournalReader::next with status Ok.
template <engine::EngineLike Engine>
void apply(Engine& engine, const RecordView& record) {
    switch (record.type) {
        case AddInstrument::kType: {
            const auto r = feed::decode<AddInstrument>(record.body);
            engine.add_instrument(r.locate, r.symbol, r.now);
            break;
        }
        case Submit::kType: {
            const auto r = feed::decode<Submit>(record.body);
            engine.submit(r.order(), r.now);
            break;
        }
        case Cancel::kType: {
            const auto r = feed::decode<Cancel>(record.body);
            engine.cancel(r.owner, r.order_id, r.now);
            break;
        }
        case Replace::kType: {
            const auto r = feed::decode<Replace>(record.body);
            engine.replace(r.owner, r.order_id, r.qty, r.price, r.now);
            break;
        }
        default:
            break;  // the reader does not hand over any other type
    }
}

struct ReplayResult {
    ReadStatus status = ReadStatus::End;  // how the journal ended
    std::uint64_t applied = 0;            // records given to the engine
    std::uint64_t skipped = 0;            // records the engine already had (see from_seq)
    std::size_t good_bytes = 0;           // the journal is sound up to here
    std::uint64_t next_sequence = 0;      // the number the next record must have

    // The journal ended after a whole record.
    [[nodiscard]] constexpr bool clean() const noexcept { return status == ReadStatus::End; }
    // The engine holds everything the journal can vouch for, and the journal
    // can be carried on from good_bytes: a clean end, or the torn tail a
    // crash leaves. Anything else needs a person to look at the file.
    [[nodiscard]] constexpr bool usable() const noexcept {
        return status == ReadStatus::End || status == ReadStatus::TornTail;
    }

    friend bool operator==(const ReplayResult&, const ReplayResult&) = default;
};

// Reads `journal` and gives its records to `engine`, stopping at the first
// one that cannot be read.
//
// `from_seq` is the number of the first record the engine has NOT yet seen: 1
// for a fresh engine, or the number a snapshot says it was taken before.
// Records below it are skipped.
//
// The journal has to cover the stretch from there on without a hole. One that
// starts after from_seq is missing the records in between, and nothing is
// applied. One that ends before from_seq is older than the snapshot the
// engine was restored from. Both are reported as BadSequence.
template <engine::EngineLike Engine>
ReplayResult replay(std::span<const std::byte> journal, Engine& engine,
                    std::uint64_t from_seq = 1) {
    JournalReader reader(journal);
    ReplayResult result;
    RecordView record;
    ReadStatus status = reader.next(record);
    const bool has_header = reader.offset() >= kFileHeaderSize;
    if (has_header && reader.first_sequence() > from_seq) {
        result.status = ReadStatus::BadSequence;
        result.good_bytes = kFileHeaderSize;
        result.next_sequence = reader.first_sequence();
        return result;
    }
    while (status == ReadStatus::Ok) {
        if (record.seq < from_seq) {
            ++result.skipped;
        } else {
            apply(engine, record);
            ++result.applied;
        }
        status = reader.next(record);
    }
    result.status = status;
    result.good_bytes = reader.offset();
    result.next_sequence = reader.next_sequence();
    if (result.usable() && has_header && result.next_sequence < from_seq) {
        result.status = ReadStatus::BadSequence;
    }
    if (!has_header && status == ReadStatus::TornTail) {
        // The file never got its header: there is nothing in it, and whoever
        // carries on starts it afresh at the record the engine is waiting for.
        result.next_sequence = from_seq;
    }
    return result;
}

// An engine whose requests are written to a journal before it acts on them.
//
// It has the engine's own interface, so anything that drives an engine can
// drive this instead.
//
// `flush_every` is how many records are gathered before the writer is told to
// flush. With 1, each request is handed to the journal's destination before
// the engine sees it, which is write-ahead in the strict sense. With more,
// the engine runs ahead of the journal by up to that many requests. That is
// "group commit", and it is only safe if the engine's OUTPUT is held back
// until commit() has been called: nobody may be told about a trade that a
// crash could still erase. The caller arranges that; a server would commit
// once per turn of its event loop, before writing to any socket.
template <engine::EngineLike Engine, class Writer>
class Journaled {
 public:
    Journaled(Engine& engine, Writer& writer, std::uint32_t flush_every = 1) noexcept
        : engine_(&engine), writer_(&writer), flush_every_(flush_every == 0 ? 1 : flush_every) {}

    bool add_instrument(Locate locate, const feed::Symbol& symbol, Nanos now) {
        record(AddInstrument{.now = now, .locate = locate, .symbol = symbol});
        return engine_->add_instrument(locate, symbol, now);
    }
    OrderId submit(const engine::NewOrder& order, Nanos now) {
        record(Submit::from(order, now));
        return engine_->submit(order, now);
    }
    bool cancel(engine::OwnerId owner, OrderId id, Nanos now) {
        record(Cancel{.now = now, .owner = owner, .order_id = id});
        return engine_->cancel(owner, id, now);
    }
    OrderId replace(engine::OwnerId owner, OrderId id, Qty qty, Price price, Nanos now) {
        record(Replace{.now = now, .owner = owner, .order_id = id, .qty = qty, .price = price});
        return engine_->replace(owner, id, qty, price, now);
    }

    // Hands everything recorded so far to the journal's destination.
    void commit() {
        writer_->flush();
        unflushed_ = 0;
    }

    [[nodiscard]] std::optional<book::Level> best(Locate locate, Side side) const {
        return engine_->best(locate, side);
    }
    template <class F>
    void for_each_level(Locate locate, Side side, F&& f) const {
        engine_->for_each_level(locate, side, std::forward<F>(f));
    }
    template <class F>
    void for_each_order(Locate locate, Side side, F&& f) const {
        engine_->for_each_order(locate, side, std::forward<F>(f));
    }
    [[nodiscard]] std::size_t open_orders() const { return engine_->open_orders(); }
    [[nodiscard]] bool listed(Locate locate) const { return engine_->listed(locate); }
    [[nodiscard]] engine::EngineStats stats() const { return engine_->stats(); }

    [[nodiscard]] Engine& engine() noexcept { return *engine_; }
    [[nodiscard]] const Engine& engine() const noexcept { return *engine_; }
    [[nodiscard]] Writer& writer() noexcept { return *writer_; }

 private:
    template <class Record>
    void record(const Record& r) {
        writer_->append(r);
        if (++unflushed_ >= flush_every_) {
            commit();
        }
    }

    Engine* engine_;
    Writer* writer_;
    std::uint32_t flush_every_;
    std::uint32_t unflushed_ = 0;
};

// A number that identifies what an engine holds: which instruments are open,
// every resting order in the order it would trade (with what it hides, what
// the market calls it and the instructions it carries), and the running
// totals.
// Two engines with the same digest are, for practical purposes, in the same
// state. It uses only the queries of the engine contract, so it works for any
// engine, and it is what the recovery tools print.
//
// `locates` bounds the walk: locates from 0 up to it are looked at.
template <engine::EngineLike Engine>
[[nodiscard]] std::uint64_t state_digest(const Engine& engine, std::uint32_t locates = 65'536) {
    using engine::detail::mix;
    std::uint64_t h = engine::detail::kHashSeed;
    for (std::uint32_t i = 0; i < locates; ++i) {
        const auto locate = static_cast<Locate>(i);
        if (!engine.listed(locate)) {
            continue;
        }
        h = mix(h, 0x100000000ULL | i);
        for (const Side side : {Side::Buy, Side::Sell}) {
            h = mix(h, static_cast<unsigned char>(side));
            engine.for_each_order(locate, side, [&h](const engine::RestingOrder& order) {
                h = mix(h, order.id);
                h = mix(h, order.owner);
                h = mix(h, order.token);
                h = mix(h, order.price);
                h = mix(h, order.qty);
                h = mix(h, order.hidden);
                h = mix(h, order.display);
                h = mix(h, order.market_ref());
                h = mix(h, order.post_only ? 1U : 0U);
                h = mix(h, static_cast<unsigned char>(order.self_match));
                return true;
            });
        }
    }
    const engine::EngineStats stats = engine.stats();
    h = mix(h, stats.accepted);
    h = mix(h, stats.rejected);
    h = mix(h, stats.cancels);
    h = mix(h, stats.replaces);
    h = mix(h, stats.trades);
    h = mix(h, stats.traded_shares);
    h = mix(h, stats.unfilled_shares);
    h = mix(h, stats.self_matches);
    h = mix(h, engine.open_orders());
    return h;
}

}  // namespace obe::journal
