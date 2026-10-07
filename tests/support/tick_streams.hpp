#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/book/types.hpp"
#include "obe/engine/feed_writer.hpp"
#include "obe/engine/reference_engine.hpp"
#include "obe/feed/parser.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/gen/rng.hpp"
#include "obe/gen/tick_gen.hpp"
#include "obe/store/codecs.hpp"
#include "obe/store/tick_codec.hpp"
#include "obe/store/tick_file.hpp"
#include "obe/types.hpp"
#include "support/printers.hpp"

// What the tick store tests share: the codecs under test, streams of ticks of
// three kinds, and a destination that keeps a store in memory.
//
// Every suite in tests/store is typed over CodecTypes and built twice:
//
//   obe_store_tests                the reference codec (RawCodec). Part of
//                                  the passing suite.
//   obe_hand_written_store_tests   built with OBE_TEST_HAND_WRITTEN defined:
//                                  DeltaCodec. Labelled needs-your-code.

namespace obe::test {

#if defined(OBE_TEST_HAND_WRITTEN)
using CodecTypes = ::testing::Types<store::DeltaCodec>;
#else
using CodecTypes = ::testing::Types<store::RawCodec>;
#endif

using store::Tick;

// The destination for a store kept in memory.
struct AppendTicksTo {
    std::vector<std::byte>* bytes;
    void operator()(std::span<const std::byte> more) const {
        bytes->insert(bytes->end(), more.begin(), more.end());
    }
};

template <class Codec>
using MemoryTickWriter = store::TickWriter<Codec, AppendTicksTo>;

// Ticks shaped like a market: a handful of securities, time moving forward in
// small steps, one side of one quote changing at a time by a little
// (obe/gen/tick_gen.hpp).
using gen::market_ticks;

// Ticks nobody would see on a feed: every field anything at all, time going
// wherever it likes. A codec has to be right about these too.
[[nodiscard]] inline std::vector<Tick> hostile_ticks(std::uint64_t seed, std::size_t count) {
    gen::SplitMix64 rng(seed);
    const auto any = [&rng](int bits) {
        // Mostly large, sometimes small, sometimes one of the edges.
        switch (rng.below(8)) {
            case 0:
                return std::uint64_t{0};
            case 1:
                return bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
            case 2:
                return rng.below(4);
            default:
                return bits == 64 ? rng.next() : rng.next() >> (64 - bits);
        }
    };
    std::vector<Tick> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Tick t;
        t.timestamp = any(64);
        t.locate = static_cast<Locate>(any(16));
        t.bbo.bid_price = static_cast<Price>(any(32));
        t.bbo.bid_qty = any(64);
        t.bbo.ask_price = static_cast<Price>(any(32));
        t.bbo.ask_qty = any(64);
        // Now and then, the same tick again, or the same security again.
        if (!out.empty() && rng.below(10) == 0) {
            t = out.back();
        } else if (!out.empty() && rng.below(5) == 0) {
            t.locate = out.back().locate;
        }
        out.push_back(t);
    }
    return out;
}

// A book listener that keeps every update.
struct TickLog {
    std::vector<Tick>* ticks;
    void on_bbo(const book::BboUpdate& update) const { ticks->push_back(update); }
};

// The market data a matching engine publishes for seeded order flow, as the
// bytes of an ITCH file.
[[nodiscard]] inline std::vector<std::byte> engine_feed(const gen::FlowConfig& cfg,
                                                        std::uint64_t commands) {
    using Engine = engine::ReferenceEngine<gen::OrderFlow, engine::ItchFeedWriter>;
    gen::OrderFlow flow(cfg);
    engine::ItchFeedWriter writer;
    const auto engine = std::make_unique<Engine>(flow, writer);
    flow.open(*engine);
    for (std::uint64_t i = 0; i < commands; ++i) {
        gen::apply(*engine, flow.next());
    }
    return writer.bytes();
}

// The best-bid-and-offer stream of a real book: that feed through the feed
// handler into a BookManager, and every update that publishes.
[[nodiscard]] inline std::vector<Tick> engine_ticks(const gen::FlowConfig& cfg,
                                                    std::uint64_t commands) {
    using Books = book::BookManager<book::OrderStore, book::PriceLevels, TickLog>;
    const std::vector<std::byte> feed = engine_feed(cfg, commands);
    std::vector<Tick> ticks;
    const auto books = std::make_unique<Books>(book::OrderStore{}, TickLog{&ticks});
    feed::ItchParser parser(*books);
    EXPECT_TRUE(parser.parse(feed).ok());
    return ticks;
}

// Writes `ticks` to a store in memory and finishes it.
template <class Codec>
[[nodiscard]] std::vector<std::byte> store_of(const std::vector<Tick>& ticks,
                                              std::uint32_t ticks_per_block) {
    std::vector<std::byte> file;
    MemoryTickWriter<Codec> writer(AppendTicksTo{&file}, ticks_per_block);
    for (const Tick& tick : ticks) {
        writer.append(tick);
    }
    writer.finish();
    return file;
}

// Every tick a reader gives for a range, and how the scan went.
template <class Codec>
struct Scanned {
    std::vector<Tick> ticks;
    store::ScanResult result;
};

template <class Codec>
[[nodiscard]] Scanned<Codec> scan_all(const store::TickReader<Codec>& reader, Nanos from = 0,
                                      Nanos to = store::kEndOfTime) {
    Scanned<Codec> out;
    out.result = reader.scan(from, to, [&out](const Tick& tick) {
        out.ticks.push_back(tick);
        return true;
    });
    return out;
}

}  // namespace obe::test
