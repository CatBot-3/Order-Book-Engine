#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "obe/engine/feed_writer.hpp"
#include "obe/engine/market_data.hpp"
#include "obe/engine/output_hash.hpp"
#include "obe/feed/handler.hpp"
#include "obe/feed/parser.hpp"
#include "support/engine_harness.hpp"
#include "support/trace_handler.hpp"

// The plumbing around the engine: the sink that turns market data into bytes,
// the two fan-out sinks, and the hashes that stand in for the full output on
// long runs.

namespace {

using namespace obe;
namespace md = engine::md;

const feed::Symbol kSymbol = feed::Symbol::from("ACME");

// Decodes a byte stream back into the messages the engine publishes.
struct Decoded : feed::HandlerBase {
    test::MdLog log;
    std::string system_events;

    void on_system_event(const feed::SystemEvent& m) { system_events.push_back(m.event_code); }
    void on_stock_directory(const feed::StockDirectory& m) { log.on_stock_directory(m); }
    void on_trading_action(const feed::TradingAction& m) { log.on_trading_action(m); }
    void on_add(const feed::AddOrder& m) { log.on_add(m); }
    void on_execute(const feed::OrderExecuted& m) { log.on_execute(m); }
    void on_cancel(const feed::OrderCancel& m) { log.on_cancel(m); }
    void on_delete(const feed::OrderDelete& m) { log.on_delete(m); }
    void on_replace(const feed::OrderReplace& m) { log.on_replace(m); }
};

// One of each message the engine can publish.
template <class Sink>
void publish_one_of_each(Sink& sink) {
    sink.on_stock_directory(md::directory(7, kSymbol, 10));
    sink.on_trading_action(md::trading(7, kSymbol, 11));
    sink.on_add(md::add(7, 12, 1, Side::Buy, 300, kSymbol, 1'000'000));
    sink.on_execute(md::execute(7, 13, 1, 100, 1));
    sink.on_cancel(md::reduce(7, 14, 1, 50));
    sink.on_replace(md::replace(7, 15, 1, 2, 400, 1'000'100));
    sink.on_delete(md::remove(7, 16, 2));
}

TEST(ItchFeedWriter, WhatItWritesParsesBackToTheSameMessages) {
    engine::ItchFeedWriter writer;
    test::MdLog sent;
    engine::TeeMarketData tee(sent, writer);
    publish_one_of_each(tee);

    Decoded decoded;
    feed::ItchParser parser(decoded);
    const feed::ParseResult result = parser.parse(writer.bytes());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.messages, 7U);
    EXPECT_EQ(decoded.log.all, sent.all);
    EXPECT_EQ(decoded.log.trace(), "RHAEXUD");
}

TEST(ItchFeedWriter, CountsMessagesAndBytes) {
    engine::ItchFeedWriter writer;
    publish_one_of_each(writer);
    writer.system_event('O', 1);
    EXPECT_EQ(writer.messages(), 8U);
    for (const char type : {'R', 'H', 'A', 'E', 'X', 'U', 'D', 'S'}) {
        EXPECT_EQ(writer.count(type), 1U) << type;
    }
    EXPECT_EQ(writer.count('P'), 0U);
    // Each message is its specification size plus the two-byte length prefix.
    EXPECT_EQ(writer.total_bytes(), 39U + 25 + 36 + 31 + 23 + 35 + 19 + 12 + 8 * 2);
    EXPECT_EQ(writer.bytes().size(), writer.total_bytes());
}

TEST(ItchFeedWriter, SystemEventsBracketTheStream) {
    engine::ItchFeedWriter writer;
    writer.system_event('O', 5);
    writer.on_add(md::add(7, 6, 1, Side::Sell, 100, kSymbol, 1'000'000));
    writer.system_event('C', 7);
    Decoded decoded;
    feed::ItchParser parser(decoded);
    ASSERT_TRUE(parser.parse(writer.bytes()).ok());
    EXPECT_EQ(decoded.system_events, "OC");
}

TEST(ItchFeedWriter, TakeHandsOverTheBufferAndKeepsCounting) {
    engine::ItchFeedWriter writer;
    writer.on_add(md::add(7, 1, 1, Side::Buy, 100, kSymbol, 1'000'000));
    const std::vector<std::byte> first = writer.take();
    EXPECT_EQ(first.size(), 38U);
    EXPECT_TRUE(writer.bytes().empty());

    writer.on_delete(md::remove(7, 2, 1));
    EXPECT_EQ(writer.bytes().size(), 21U);
    EXPECT_EQ(writer.messages(), 2U);
    EXPECT_EQ(writer.total_bytes(), 59U);

    // The two pieces, back to back, are one valid stream.
    std::vector<std::byte> whole = first;
    whole.insert(whole.end(), writer.bytes().begin(), writer.bytes().end());
    test::TraceHandler trace;
    feed::ItchParser parser(trace);
    ASSERT_TRUE(parser.parse(whole).ok());
    EXPECT_EQ(trace.trace, "AD");
}

TEST(TeeMarketData, DeliversToTheFirstSinkThenTheSecond) {
    // Both sinks append to one string, so the order of delivery is visible.
    struct Tagger : feed::HandlerBase {
        std::string* out;
        char tag;
        void note(char type) {
            out->push_back(type);
            out->push_back(tag);
        }
        void on_stock_directory(const feed::StockDirectory&) { note('R'); }
        void on_trading_action(const feed::TradingAction&) { note('H'); }
        void on_add(const feed::AddOrder&) { note('A'); }
        void on_execute(const feed::OrderExecuted&) { note('E'); }
        void on_cancel(const feed::OrderCancel&) { note('X'); }
        void on_delete(const feed::OrderDelete&) { note('D'); }
        void on_replace(const feed::OrderReplace&) { note('U'); }
    };
    std::string order;
    Tagger one{{}, &order, '1'};
    Tagger two{{}, &order, '2'};
    engine::TeeMarketData tee(one, two);
    publish_one_of_each(tee);
    EXPECT_EQ(order, "R1R2H1H2A1A2E1E2X1X2U1U2D1D2");
}

TEST(TeeReports, DeliversEveryReportToBothInOrder) {
    test::ReportLog one;
    test::ReportLog two;
    engine::TeeReports tee(one, two);
    tee.on_accepted({.order_id = 1});
    tee.on_executed({.order_id = 2});
    tee.on_cancelled({.order_id = 3});
    tee.on_replaced({.old_id = 4});
    tee.on_rejected({.order_id = 5});
    EXPECT_EQ(one.all.size(), 5U);
    EXPECT_EQ(one.all, two.all);
    EXPECT_EQ(one.all[3], (test::Report{engine::Replaced{.old_id = 4}}));
}

TEST(ByteHasher, DoesNotDependOnHowTheStreamIsCutUp) {
    engine::ItchFeedWriter writer;
    publish_one_of_each(writer);
    const std::span<const std::byte> all = writer.bytes();

    engine::ByteHasher whole;
    whole.update(all);
    engine::ByteHasher pieces;
    for (std::size_t at = 0; at < all.size(); at += 7) {
        pieces.update(all.subspan(at, std::min<std::size_t>(7, all.size() - at)));
    }
    EXPECT_EQ(whole.hash(), pieces.hash());
    EXPECT_EQ(whole.bytes(), all.size());
    EXPECT_EQ(pieces.bytes(), all.size());
}

TEST(ByteHasher, ChangesWithAnyByte) {
    std::vector<std::byte> data(64, std::byte{0});
    engine::ByteHasher base;
    base.update(data);
    for (std::size_t i = 0; i < data.size(); ++i) {
        std::vector<std::byte> changed = data;
        changed[i] = std::byte{1};
        engine::ByteHasher h;
        h.update(changed);
        EXPECT_NE(h.hash(), base.hash()) << "byte " << i;
    }
    engine::ByteHasher empty;
    EXPECT_NE(empty.hash(), base.hash());
}

// The hash of one report after a hasher has seen it.
template <class R>
std::uint64_t hash_of(void (engine::ReportHasher::*on)(const R&) noexcept, const R& report) {
    engine::ReportHasher h;
    (h.*on)(report);
    return h.hash();
}

TEST(ReportHasher, ChangesWithEveryFieldOfEveryReport) {
    using engine::ReportHasher;
    {
        const engine::Accepted base{};
        const auto h = [](const engine::Accepted& r) {
            return hash_of(&ReportHasher::on_accepted, r);
        };
        std::vector<engine::Accepted> variants(10, base);
        variants[0].order_id = 1;
        variants[1].owner = 1;
        variants[2].token = 1;
        variants[3].locate = 1;
        variants[4].side = Side::Sell;
        variants[5].qty = 1;
        variants[6].price = 1;
        variants[7].kind = engine::OrderKind::Market;
        variants[8].tif = engine::TimeInForce::FillOrKill;
        variants[9].timestamp = 1;
        for (std::size_t i = 0; i < variants.size(); ++i) {
            EXPECT_NE(h(variants[i]), h(base)) << "Accepted field " << i;
        }
    }
    {
        const engine::Executed base{};
        const auto h = [](const engine::Executed& r) {
            return hash_of(&ReportHasher::on_executed, r);
        };
        std::vector<engine::Executed> variants(9, base);
        variants[0].order_id = 1;
        variants[1].owner = 1;
        variants[2].token = 1;
        variants[3].qty = 1;
        variants[4].price = 1;
        variants[5].leaves = 1;
        variants[6].match_number = 1;
        variants[7].liquidity = engine::Liquidity::Removed;
        variants[8].timestamp = 1;
        for (std::size_t i = 0; i < variants.size(); ++i) {
            EXPECT_NE(h(variants[i]), h(base)) << "Executed field " << i;
        }
    }
    {
        const engine::Cancelled base{};
        const auto h = [](const engine::Cancelled& r) {
            return hash_of(&ReportHasher::on_cancelled, r);
        };
        std::vector<engine::Cancelled> variants(7, base);
        variants[0].order_id = 1;
        variants[1].owner = 1;
        variants[2].token = 1;
        variants[3].qty = 1;
        variants[4].leaves = 1;
        variants[5].reason = engine::CancelReason::FillOrKill;
        variants[6].timestamp = 1;
        for (std::size_t i = 0; i < variants.size(); ++i) {
            EXPECT_NE(h(variants[i]), h(base)) << "Cancelled field " << i;
        }
    }
    {
        const engine::Replaced base{};
        const auto h = [](const engine::Replaced& r) {
            return hash_of(&ReportHasher::on_replaced, r);
        };
        std::vector<engine::Replaced> variants(8, base);
        variants[0].old_id = 1;
        variants[1].new_id = 1;
        variants[2].owner = 1;
        variants[3].token = 1;
        variants[4].qty = 1;
        variants[5].price = 1;
        variants[6].kept_priority = true;
        variants[7].timestamp = 1;
        for (std::size_t i = 0; i < variants.size(); ++i) {
            EXPECT_NE(h(variants[i]), h(base)) << "Replaced field " << i;
        }
    }
    {
        const engine::Rejected base{};
        const auto h = [](const engine::Rejected& r) {
            return hash_of(&ReportHasher::on_rejected, r);
        };
        std::vector<engine::Rejected> variants(5, base);
        variants[0].owner = 1;
        variants[1].token = 1;
        variants[2].order_id = 1;
        variants[3].reason = engine::RejectReason::NotOwner;
        variants[4].timestamp = 1;
        for (std::size_t i = 0; i < variants.size(); ++i) {
            EXPECT_NE(h(variants[i]), h(base)) << "Rejected field " << i;
        }
    }
}

TEST(ReportHasher, DependsOnTheOrderAndTheKindOfReports) {
    const engine::Cancelled a{.order_id = 1};
    const engine::Cancelled b{.order_id = 2};
    engine::ReportHasher ab;
    ab.on_cancelled(a);
    ab.on_cancelled(b);
    engine::ReportHasher ba;
    ba.on_cancelled(b);
    ba.on_cancelled(a);
    EXPECT_NE(ab.hash(), ba.hash());
    EXPECT_EQ(ab.reports(), 2U);

    // Two reports with the same numbers in them but of different kinds.
    engine::ReportHasher cancelled;
    cancelled.on_cancelled({});
    engine::ReportHasher rejected;
    rejected.on_rejected({});
    EXPECT_NE(cancelled.hash(), rejected.hash());
}

}  // namespace
