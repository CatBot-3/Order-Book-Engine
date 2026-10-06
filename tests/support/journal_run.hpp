#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "obe/engine/concepts.hpp"
#include "obe/engine/feed_writer.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/journal/reader.hpp"
#include "obe/journal/records.hpp"
#include "obe/journal/replay.hpp"
#include "obe/journal/writer.hpp"
#include "obe/types.hpp"
#include "support/engine_harness.hpp"

// What the journal tests share: an engine that is being journaled while seeded
// flow trades on it, a plain engine to recover into, and a comparison of two
// engines that looks at everything the contract lets it see.

namespace obe::test {

// The destination for a journal kept in memory.
struct AppendTo {
    std::vector<std::byte>* bytes;
    void operator()(std::span<const std::byte> more) const {
        bytes->insert(bytes->end(), more.begin(), more.end());
    }
};

using MemoryWriter = journal::JournalWriter<AppendTo>;

// The original: seeded flow drives an engine through a journal. Everything
// the engine says is kept, and so is the size of the journal after every
// request, so that a test can cut it at any record.
template <class Impl>
struct Recorded {
    using Reports = engine::TeeReports<gen::OrderFlow, ReportLog>;
    using Engine = typename Impl::template Engine<Reports, MdLog>;
    using Wrapped = journal::Journaled<Engine, MemoryWriter>;

    explicit Recorded(const gen::FlowConfig& cfg, std::uint32_t flush_every = 1)
        : flow(cfg),
          reports(flow, log),
          engine(std::make_unique<Engine>(reports, messages)),
          writer(AppendTo{&bytes}),
          journaled(*engine, writer, flush_every) {
        flow.open(journaled);
        opened = writer.records();
        digests.resize(opened + 1, 0);
        digests[opened] = journal::state_digest(*engine, kLocates);
    }

    // One request. Returns it, so that the same one can be given elsewhere.
    gen::Command step() {
        const gen::Command command = flow.next();
        gen::apply(journaled, command);
        digests.push_back(journal::state_digest(*engine, kLocates));
        return command;
    }
    void run(std::uint64_t commands) {
        for (std::uint64_t i = 0; i < commands; ++i) {
            step();
        }
    }

    // ends()[r] is the size of the journal with r records in it. Found by
    // walking the journal, so it does not depend on when the writer flushed.
    [[nodiscard]] std::vector<std::size_t> ends() const {
        std::vector<std::size_t> out{journal::kFileHeaderSize};
        journal::JournalReader reader(bytes);
        journal::RecordView record;
        while (reader.next(record) == journal::ReadStatus::Ok) {
            out.push_back(reader.offset());
        }
        return out;
    }

    gen::OrderFlow flow;
    ReportLog log;
    MdLog messages;
    Reports reports;
    std::unique_ptr<Engine> engine;
    std::vector<std::byte> bytes;  // the journal
    MemoryWriter writer;
    Wrapped journaled;
    // The number of records that open instruments, written before any
    // request, and what the engine held after each record from then on:
    // digests[r] for r >= opened.
    std::uint64_t opened = 0;
    std::vector<std::uint64_t> digests;

    static constexpr std::uint32_t kLocates = 64;  // the flows use far fewer
};

// A fresh engine with its own logs: what recovery fills.
template <class Impl>
struct Replica {
    using Engine = typename Impl::template Engine<ReportLog, MdLog>;

    Replica() : engine(std::make_unique<Engine>(log, messages)) {}

    ReportLog log;
    MdLog messages;
    std::unique_ptr<Engine> engine;
};

// Two engines hold the same thing, as far as the contract lets anyone see:
// the same instruments, the same orders in the same places, the same totals.
template <class A, class B>
::testing::AssertionResult same_state(const A& a, const B& b, std::uint32_t locates = 64) {
    for (std::uint32_t i = 0; i < locates; ++i) {
        const auto locate = static_cast<Locate>(i);
        if (a.listed(locate) != b.listed(locate)) {
            return ::testing::AssertionFailure() << "locate " << i << " is open in only one";
        }
        for (const Side side : {Side::Buy, Side::Sell}) {
            if (orders_of(a, locate, side) != orders_of(b, locate, side)) {
                return ::testing::AssertionFailure()
                       << "the " << (side == Side::Buy ? "bids" : "asks") << " of locate " << i
                       << " differ";
            }
            if (levels_of(a, locate, side) != levels_of(b, locate, side)) {
                return ::testing::AssertionFailure() << "the levels of locate " << i << " differ";
            }
        }
    }
    if (!(engine::EngineStats(a.stats()) == engine::EngineStats(b.stats()))) {
        return ::testing::AssertionFailure() << "the statistics differ";
    }
    if (a.open_orders() != b.open_orders()) {
        return ::testing::AssertionFailure() << "the number of open orders differs";
    }
    return ::testing::AssertionSuccess();
}

}  // namespace obe::test
