#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "obe/book/bbo_hash.hpp"
#include "obe/book/book_manager.hpp"
#include "obe/book/implementations.hpp"
#include "obe/engine/feed_writer.hpp"
#include "obe/engine/reference_engine.hpp"
#include "obe/feed/codec.hpp"
#include "obe/feed/parser.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/gen/synthetic_feed.hpp"
#include "obe/pipeline/events.hpp"
#include "obe/pipeline/pipeline.hpp"
#include "obe/pipeline/top_of_book.hpp"
#include "obe/util/mutex_queue.hpp"
#include "support/printers.hpp"
#include "support/queues.hpp"
#include "support/threads.hpp"

// The three-thread pipeline.
//
// The claim under test is in the header of obe/pipeline/pipeline.hpp: putting
// the parser, the book and the consumer on separate threads changes when
// things happen and nothing else. Every test here therefore compares the
// pipeline with the plain single-threaded replay of the same bytes: the same
// best-bid-and-offer stream for every security, the same counters, the same
// final book.

namespace {

using namespace obe;

using Reference = book::BookManager<book::OrderStore, book::PriceLevels, book::BboHasher>;

struct Replay {
    std::unique_ptr<Reference> manager = std::make_unique<Reference>();
    feed::ParseResult parse;
};

Replay single_threaded(std::span<const std::byte> stream) {
    Replay out;
    feed::ItchParser parser(*out.manager);
    out.parse = parser.parse(stream);
    return out;
}

// A stream with every message type, from the raw generator.
std::vector<std::byte> synthetic(std::uint64_t seed, std::uint64_t messages) {
    return gen::make_synthetic_feed(
        {.seed = seed, .symbols = 40, .messages = messages, .target_live_orders = 3'000});
}

// A stream a matching engine published: price-time priority, never crossed.
std::vector<std::byte> engine_feed(std::uint64_t seed, std::uint64_t commands) {
    using Engine = engine::ReferenceEngine<gen::OrderFlow, engine::ItchFeedWriter>;
    gen::OrderFlow flow({.seed = seed, .symbols = 25, .target_live_orders = 2'000});
    engine::ItchFeedWriter writer;
    const auto engine = std::make_unique<Engine>(flow, writer);
    flow.open(*engine);
    for (std::uint64_t i = 0; i < commands; ++i) {
        gen::apply(*engine, flow.next());
    }
    return writer.take();
}

template <class Kind>
class PipelineTest : public ::testing::Test {
 protected:
    // The pipeline on this kind of channel.
    static pipeline::PipelineResult run(std::span<const std::byte> stream,
                                        const pipeline::PipelineConfig& cfg = {}) {
        return pipeline::run_pipeline<Kind::template Channel>(stream, cfg);
    }

    static void expect_same_as_single_threaded(std::span<const std::byte> stream,
                                               const pipeline::PipelineConfig& cfg = {}) {
        const Replay expected = single_threaded(stream);
        const pipeline::PipelineResult got = run(stream, cfg);

        EXPECT_EQ(got.parse.status, expected.parse.status);
        EXPECT_EQ(got.parse.messages, expected.parse.messages);
        EXPECT_EQ(got.parse.offset, expected.parse.offset);
        EXPECT_EQ(got.counters, expected.manager->counters());
        EXPECT_EQ(got.stats, expected.manager->stats());

        // The consumer thread received every update, in order, for every
        // security: equal hashes mean equal streams.
        const book::BboHasher& want = expected.manager->listener();
        ASSERT_NE(got.hasher, nullptr);
        EXPECT_EQ(got.hasher->total_events(), want.total_events());
        EXPECT_EQ(got.updates, want.total_events());
        EXPECT_EQ(got.hasher->combined(), want.combined());
        for (std::size_t i = 0; i < book::BboHasher::kLocates; ++i) {
            const auto locate = static_cast<Locate>(i);
            ASSERT_EQ(got.hasher->events(locate), want.events(locate)) << "locate " << i;
            ASSERT_EQ(got.hasher->hash(locate), want.hash(locate)) << "locate " << i;
        }

        // And the book thread ended with the same book.
        const book::Audit audit = expected.manager->audit();
        EXPECT_EQ(got.audit.levels, audit.levels);
        EXPECT_EQ(got.audit.level_shares, audit.level_shares);
        EXPECT_EQ(got.audit.open_shares, audit.open_shares);
        EXPECT_EQ(got.audit.open_orders, audit.open_orders);
        EXPECT_EQ(got.audit.empty_levels, 0U);
    }
};
TYPED_TEST_SUITE(PipelineTest, test::ChannelKinds);

TYPED_TEST(PipelineTest, PublishesTheSameStreamAsASingleThread) {
    for (const std::uint64_t seed : {1U, 2U, 3U}) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        this->expect_same_as_single_threaded(synthetic(seed, 60'000));
        ASSERT_FALSE(this->HasFatalFailure());
    }
}

TYPED_TEST(PipelineTest, PublishesTheSameStreamForAMatchingEnginesFeed) {
    this->expect_same_as_single_threaded(engine_feed(5, 60'000));
}

TYPED_TEST(PipelineTest, WorksWhenTheQueuesHoldAlmostNothing) {
    // With one or two slots every stage is waiting on a neighbour nearly all
    // the time, so every wait path is taken thousands of times.
    const std::vector<std::byte> stream = synthetic(4, 30'000);
    for (const std::size_t capacity : {1U, 2U, 3U}) {
        SCOPED_TRACE("capacity " + std::to_string(capacity));
        this->expect_same_as_single_threaded(
            stream, {.event_capacity = capacity, .update_capacity = capacity});
        ASSERT_FALSE(this->HasFatalFailure());
    }
}

TYPED_TEST(PipelineTest, WorksWhenOneQueueIsMuchSmallerThanTheOther) {
    const std::vector<std::byte> stream = synthetic(6, 60'000);
    this->expect_same_as_single_threaded(stream, {.event_capacity = 4096, .update_capacity = 1});
    this->expect_same_as_single_threaded(stream, {.event_capacity = 1, .update_capacity = 4096});
}

TYPED_TEST(PipelineTest, GivesTheSameAnswerEveryTime) {
    const std::vector<std::byte> stream = synthetic(7, 60'000);
    const pipeline::PipelineResult first = this->run(stream);
    for (int i = 0; i < 4; ++i) {
        const pipeline::PipelineResult again = this->run(stream);
        ASSERT_EQ(again.hasher->combined(), first.hasher->combined()) << "run " << i;
        ASSERT_EQ(again.events, first.events);
        ASSERT_EQ(again.updates, first.updates);
    }
}

TYPED_TEST(PipelineTest, AnEmptyStreamRunsAndPublishesNothing) {
    const pipeline::PipelineResult got = this->run({});
    EXPECT_TRUE(got.parse.ok());
    EXPECT_EQ(got.parse.messages, 0U);
    EXPECT_EQ(got.events, 0U);
    EXPECT_EQ(got.updates, 0U);
    EXPECT_EQ(got.hasher->total_events(), 0U);
}

TYPED_TEST(PipelineTest, OnlyBookMessagesCrossTheFirstQueue) {
    const std::vector<std::byte> stream = synthetic(8, 50'000);
    const pipeline::PipelineResult got = this->run(stream);
    // Count the types the book handles with the plain parser.
    struct Counter : feed::HandlerBase {
        std::uint64_t book_messages = 0;
        void on_stock_directory(const feed::StockDirectory&) { ++book_messages; }
        void on_trading_action(const feed::TradingAction&) { ++book_messages; }
        void on_add(const feed::AddOrder&) { ++book_messages; }
        void on_execute(const feed::OrderExecuted&) { ++book_messages; }
        void on_execute_with_price(const feed::OrderExecutedWithPrice&) { ++book_messages; }
        void on_cancel(const feed::OrderCancel&) { ++book_messages; }
        void on_delete(const feed::OrderDelete&) { ++book_messages; }
        void on_replace(const feed::OrderReplace&) { ++book_messages; }
    } counter;
    feed::ItchParser parser(counter);
    ASSERT_TRUE(parser.parse(stream).ok());
    EXPECT_EQ(got.events, counter.book_messages);
    EXPECT_LT(got.events, got.parse.messages) << "the stream has messages the book ignores";
}

TYPED_TEST(PipelineTest, ATruncatedStreamIsReportedAndTheGoodPartStillApplied) {
    std::vector<std::byte> stream = synthetic(9, 20'000);
    stream.resize(stream.size() - 7);  // cut the last message short
    const Replay expected = single_threaded(stream);
    ASSERT_EQ(expected.parse.status, feed::ParseStatus::TruncatedMessage);
    this->expect_same_as_single_threaded(stream);
}

TYPED_TEST(PipelineTest, AnUnknownMessageTypeStopsTheParserThreadCleanly) {
    std::vector<std::byte> stream = synthetic(10, 20'000);
    // Append one framed message of a type that does not exist, then more
    // valid data that must not be applied.
    const std::vector<std::byte> tail = synthetic(11, 1'000);
    stream.push_back(std::byte{0});
    stream.push_back(std::byte{3});
    stream.push_back(static_cast<std::byte>('z'));
    stream.push_back(std::byte{0});
    stream.push_back(std::byte{0});
    stream.insert(stream.end(), tail.begin(), tail.end());
    const Replay expected = single_threaded(stream);
    ASSERT_EQ(expected.parse.status, feed::ParseStatus::UnknownType);
    this->expect_same_as_single_threaded(stream);
}

TYPED_TEST(PipelineTest, TheBoardEndsWithEachSecuritysLastTopOfBook) {
    const std::vector<std::byte> stream = engine_feed(12, 60'000);
    const Replay expected = single_threaded(stream);
    pipeline::TopOfBookBoard board;
    const pipeline::PipelineResult got = this->run(stream, {.board = &board});
    ASSERT_EQ(got.hasher->combined(), expected.manager->listener().combined());

    std::uint64_t published = 0;
    for (std::size_t i = 0; i < pipeline::TopOfBookBoard::kLocates; ++i) {
        const auto locate = static_cast<Locate>(i);
        const std::uint64_t updates = expected.manager->listener().events(locate);
        ASSERT_EQ(board.updates(locate), updates) << "locate " << i;
        published += updates;
        if (updates != 0) {
            // The book publishes on every change, so the last thing published
            // is what the book shows now.
            ASSERT_EQ(board.read(locate).bbo, expected.manager->book(locate).bbo())
                << "locate " << i;
            ASSERT_EQ(board.read(locate).locate, locate);
        }
    }
    EXPECT_EQ(published, got.updates);
}

TYPED_TEST(PipelineTest, ReadersCanWatchTheBoardWhileThePipelineRuns) {
    const std::vector<std::byte> stream = engine_feed(13, 80'000);
    pipeline::TopOfBookBoard board;
    std::atomic<bool> done{false};
    std::atomic<bool> reading{false};
    std::uint64_t wrong_locate = 0;
    std::uint64_t backwards = 0;
    std::uint64_t crossed = 0;
    std::uint64_t reads = 0;

    test::Abort abort;
    test::Worker reader(abort, [&] {
        std::array<Nanos, 26> last{};
        reading.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire) && !abort.raised()) {
            for (Locate locate = 1; locate <= 25; ++locate) {
                const book::BboUpdate seen = board.read(locate);
                ++reads;
                if (seen.timestamp == 0) {
                    continue;  // nothing published for it yet
                }
                wrong_locate += seen.locate != locate ? 1U : 0U;
                backwards += seen.timestamp < last[locate] ? 1U : 0U;
                // A matching engine's book is never crossed. A snapshot that
                // mixed the bid of one update with the ask of another could be.
                crossed += seen.bbo.locked() || seen.bbo.crossed() ? 1U : 0U;
                last[locate] = seen.timestamp;
            }
        }
    });
    // If the pipeline throws, the reader must still be told to stop.
    const test::StopOnExit stop(abort);
    // The pipeline can finish before a new thread has even been scheduled.
    // Wait until the reader is running, or it would watch nothing.
    while (!reading.load(std::memory_order_acquire) && !abort.raised()) {
        std::this_thread::yield();
    }
    const pipeline::PipelineResult got = this->run(stream, {.board = &board});
    done.store(true, std::memory_order_release);
    reader.join();

    EXPECT_EQ(wrong_locate, 0U);
    EXPECT_EQ(backwards, 0U);
    EXPECT_EQ(crossed, 0U);
    EXPECT_GT(reads, 0U);
    EXPECT_EQ(got.hasher->combined(), single_threaded(stream).manager->listener().combined());
}

TYPED_TEST(PipelineTest, CountsTheWaitsOnEachQueue) {
    // One slot in the first queue: the parser thread has to wait for the book
    // thread over and over.
    const std::vector<std::byte> stream = synthetic(14, 30'000);
    const pipeline::PipelineResult got =
        this->run(stream, {.event_capacity = 1, .update_capacity = 4096});
    EXPECT_GT(got.event_channel.push_waits + got.event_channel.pop_waits, 0U);
}

TYPED_TEST(PipelineTest, PinningIsReportedPerThread) {
    const std::vector<std::byte> stream = synthetic(15, 5'000);
    const pipeline::PipelineResult unpinned = this->run(stream);
    EXPECT_FALSE(unpinned.pinned[0] || unpinned.pinned[1] || unpinned.pinned[2]);
    // CPU 0 always exists. Whether the process is allowed on it is up to the
    // machine, so only the request that cannot succeed is checked strictly.
    const pipeline::PipelineResult impossible = this->run(stream, {.cpus = {1'000'000, -1, -1}});
    EXPECT_FALSE(impossible.pinned[0]);
    EXPECT_EQ(impossible.hasher->combined(), unpinned.hasher->combined());
}

// --- Failure -----------------------------------------------------------------

// A channel whose producer side throws after a set number of pushes. What the
// pipeline must do then is stop every thread and hand the exception back.
struct Boom : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <class T>
class FailingChannel : public util::MutexQueue<T> {
 public:
    using util::MutexQueue<T>::MutexQueue;

    template <class U>
    bool push(U&& value) {
        if (++pushes_ == kFailAt) {
            throw Boom("push failed");
        }
        return util::MutexQueue<T>::push(std::forward<U>(value));
    }

 private:
    static constexpr int kFailAt = 500;
    int pushes_ = 0;
};

TEST(PipelineFailure, AStageThatThrowsStopsTheOthersAndTheExceptionComesBack) {
    const std::vector<std::byte> stream = synthetic(16, 50'000);
    // Tiny queues, so that the other stages are waiting when it happens.
    EXPECT_THROW(static_cast<void>(pipeline::run_pipeline<FailingChannel>(
                     stream, {.event_capacity = 2, .update_capacity = 2})),
                 Boom);
    // Reaching this line is the other half of the test: nothing hung, and no
    // thread was left running to end the process.
}

// --- The event encoding ------------------------------------------------------

TEST(BookEvent, EveryBookMessageSurvivesTheTripThroughAnEvent) {
    // Decode a stream into events, apply the events to one book, and apply
    // the original messages to another. The books must agree on everything.
    const std::vector<std::byte> stream = synthetic(17, 80'000);
    const Replay direct = single_threaded(stream);

    const auto through_events = std::make_unique<Reference>();
    std::uint64_t events = 0;
    pipeline::EventEncoder encoder([&](const pipeline::BookEvent& e) {
        ++events;
        pipeline::apply_event(e, *through_events);
    });
    feed::ItchParser parser(encoder);
    ASSERT_TRUE(parser.parse(stream).ok());

    EXPECT_GT(events, 60'000U);
    EXPECT_EQ(through_events->counters(), direct.manager->counters());
    EXPECT_EQ(through_events->stats(), direct.manager->stats());
    EXPECT_EQ(through_events->listener().combined(), direct.manager->listener().combined());
    for (std::size_t i = 0; i < Reference::kLocates; ++i) {
        const auto locate = static_cast<Locate>(i);
        ASSERT_EQ(through_events->symbol(locate), direct.manager->symbol(locate));
        ASSERT_EQ(through_events->listed(locate), direct.manager->listed(locate));
        ASSERT_EQ(through_events->book(locate).trading_state(),
                  direct.manager->book(locate).trading_state());
        ASSERT_EQ(through_events->book(locate).executed_shares(),
                  direct.manager->book(locate).executed_shares());
    }
}

TEST(BookEvent, CarriesTheFieldsOfEachMessage) {
    std::vector<pipeline::BookEvent> events;
    pipeline::EventEncoder encoder([&](const pipeline::BookEvent& e) { events.push_back(e); });

    encoder.on_add(feed::AddOrder{.hdr = {.locate = 9, .tracking = 1, .timestamp = 123},
                                  .order_ref = 77,
                                  .side = Side::Sell,
                                  .shares = 300,
                                  .price = 1'000'000,
                                  .attributed = true});
    encoder.on_replace(feed::OrderReplace{.hdr = {.locate = 9, .tracking = 0, .timestamp = 124},
                                          .orig_order_ref = 77,
                                          .new_order_ref = 78,
                                          .shares = 200,
                                          .price = 1'000'100});
    encoder.on_execute_with_price(
        feed::OrderExecutedWithPrice{.hdr = {.locate = 9, .tracking = 0, .timestamp = 125},
                                     .order_ref = 78,
                                     .shares = 50,
                                     .match_number = 5,
                                     .printable = 'N',
                                     .price = 999'900});
    encoder.on_stock_directory(feed::StockDirectory{
        .hdr = {.locate = 9, .tracking = 0, .timestamp = 1}, .stock = feed::Symbol::from("ACME")});
    // Not a book message: it has no callback in the encoder and emits nothing.
    encoder.on_trade(feed::Trade{});

    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0], (pipeline::BookEvent{.ref = 77,
                                              .timestamp = 123,
                                              .price = 1'000'000,
                                              .shares = 300,
                                              .locate = 9,
                                              .type = 'F',
                                              .aux = 'S'}));
    EXPECT_EQ(events[1], (pipeline::BookEvent{.ref = 77,
                                              .new_ref = 78,
                                              .timestamp = 124,
                                              .price = 1'000'100,
                                              .shares = 200,
                                              .locate = 9,
                                              .type = 'U'}));
    EXPECT_EQ(events[2].type, 'C');
    EXPECT_EQ(events[2].aux, 'N');
    EXPECT_EQ(events[2].price, 999'900U);
    EXPECT_EQ(events[3].type, 'R');

    // And an unknown type byte is ignored on the way back in.
    Reference book;
    pipeline::apply_event(pipeline::BookEvent{.type = '?'}, book);
    EXPECT_EQ(book.counters(), book::Counters{});
}

}  // namespace
