#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <thread>

#include "obe/book/bbo_hash.hpp"
#include "obe/book/book_manager.hpp"
#include "obe/book/implementations.hpp"
#include "obe/book/types.hpp"
#include "obe/feed/parser.hpp"
#include "obe/pipeline/events.hpp"
#include "obe/pipeline/top_of_book.hpp"
#include "obe/util/cpu.hpp"
#include "obe/util/queue_concepts.hpp"

// The replay as three threads joined by two queues.
//
//   parser thread ──(BookEvent)──> book thread ──(BboUpdate)──> consumer thread
//   framing, decode                order store, levels           hash, statistics
//
// Each stage has one writer for everything it owns. Nothing is shared between
// two threads except the queues (and, if asked for, the top-of-book board),
// so the book logic is the single-threaded code of phase 2, unchanged and
// testable on its own. That is design decision 7 of the spec.
//
// The queue type is a template parameter, so the same pipeline runs on the
// mutex queue and on the lock-free ring and the two can be measured against
// each other.
//
// Whether this is faster than doing all three steps on one thread is an open
// question that phase 6 exists to answer. Handing a message to another core
// costs something: the cache line it was written to has to travel. When a
// message takes tens of nanoseconds to process, that can cost more than the
// work it moves. pipeline_bench measures both.
//
// The output is deterministic regardless of how the threads interleave: each
// queue preserves order, each stage has one input, and no stage reads a
// clock. The consumer's hash must therefore equal a single-threaded replay's.

namespace obe::pipeline {

struct PipelineConfig {
    std::size_t event_capacity = std::size_t{1} << 16;   // parser to book
    std::size_t update_capacity = std::size_t{1} << 16;  // book to consumer
    // CPU for the parser, book and consumer threads; -1 leaves one unpinned.
    std::array<int, 3> cpus{-1, -1, -1};
    std::size_t reserve = 0;  // pre-size the order store, as --reserve does
    // If set, the book thread also publishes every update here, for readers
    // on other threads. Must outlive the call.
    TopOfBookBoard* board = nullptr;
};

struct PipelineResult {
    feed::ParseResult parse;    // from the parser thread
    std::uint64_t events = 0;   // BookEvents handed to the book thread
    std::uint64_t updates = 0;  // BboUpdates handed to the consumer thread
    // From just before the threads start to just after the last one is
    // joined. It includes creating and joining three threads, which is tens of
    // microseconds and does not matter against a run of a second or more.
    double seconds = 0;
    book::Counters counters;
    book::Stats stats;
    book::Audit audit;
    std::unique_ptr<book::BboHasher> hasher;  // every update the consumer received
    util::ChannelStats event_channel;
    util::ChannelStats update_channel;
    std::array<bool, 3> pinned{false, false, false};
};

// Replays `buf` through the three-stage pipeline and returns when all three
// threads have finished.
//
// ChannelT<T> is a util::Channel, for example util::MutexQueue or a
// SpinChannel over a ring. If a stage throws, the other stages are cancelled,
// every thread is joined, and the exception is rethrown here.
template <template <class> class ChannelT, class Impl = book::ReferenceImpl>
[[nodiscard]] PipelineResult run_pipeline(std::span<const std::byte> buf,
                                          const PipelineConfig& cfg = {}) {
    using Events = ChannelT<BookEvent>;
    using Updates = ChannelT<book::BboUpdate>;
    static_assert(util::Channel<Events>);
    static_assert(util::Channel<Updates>);

    // The book's listener, on the book thread: every change of a best bid or
    // offer goes to the board and into the queue to the consumer.
    struct Forwarder {
        Updates* out = nullptr;
        TopOfBookBoard* board = nullptr;
        std::uint64_t pushed = 0;

        void on_bbo(const book::BboUpdate& update) {
            if (board != nullptr) {
                board->publish(update);
            }
            if (out->push(update)) {
                ++pushed;
            }
        }
    };
    using Manager = book::BookManager<typename Impl::Store, typename Impl::Levels, Forwarder>;

    Events events(cfg.event_capacity);
    Updates updates(cfg.update_capacity);
    const auto manager = std::make_unique<Manager>(
        book::make_store<typename Impl::Store>(cfg.reserve), Forwarder{&updates, cfg.board});

    PipelineResult result;
    result.hasher = std::make_unique<book::BboHasher>();
    std::array<std::exception_ptr, 3> errors;

    // Runs a stage's body on its thread. A stage that throws must not leave
    // its neighbours waiting on a queue it will never touch again.
    const auto run_stage = [&](std::size_t index, auto body) {
        try {
            if (cfg.cpus[index] >= 0) {
                result.pinned[index] = util::pin_to_cpu(cfg.cpus[index]);
            }
            body();
        } catch (...) {
            errors[index] = std::current_exception();
            events.cancel();
            updates.cancel();
        }
    };

    const auto parser_stage = [&] {
        std::uint64_t pushed = 0;
        bool open = true;
        EventEncoder encoder([&](const BookEvent& e) {
            if (open && events.push(e)) {
                ++pushed;
            } else {
                open = false;  // cancelled: stop reading
            }
        });
        feed::ItchParser parser(encoder);
        feed::FrameReader reader(buf);
        feed::Frame frame;
        feed::ParseResult parsed;
        while (open && !reader.done()) {
            parsed.offset = reader.offset();
            parsed.status = reader.next(frame);
            if (parsed.status != feed::ParseStatus::Ok) [[unlikely]] {
                break;
            }
            parsed.status = parser.dispatch(frame);
            if (parsed.status != feed::ParseStatus::Ok) [[unlikely]] {
                break;
            }
            ++parsed.messages;
        }
        if (parsed.ok()) {
            parsed.offset = reader.offset();
        }
        result.parse = parsed;
        result.events = pushed;
        // Whatever was read before a bad frame is still delivered, exactly as
        // a single-threaded parse would have applied it.
        events.close();
    };

    const auto book_stage = [&] {
        BookEvent event;
        while (events.pop(event)) {
            apply_event(event, *manager);
        }
        result.updates = manager->listener().pushed;
        updates.close();
    };

    const auto consumer_stage = [&] {
        book::BboUpdate update;
        while (updates.pop(update)) {
            result.hasher->on_bbo(update);
        }
    };

    // Joins whatever was started, on every way out of the block below.
    struct Joiner {
        std::array<std::thread, 3> threads;
        ~Joiner() {
            for (std::thread& t : threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }
    };

    const auto start = std::chrono::steady_clock::now();
    {
        Joiner joiner;
        try {
            // Downstream first, so every stage finds its consumer already waiting.
            joiner.threads[2] = std::thread(run_stage, std::size_t{2}, consumer_stage);
            joiner.threads[1] = std::thread(run_stage, std::size_t{1}, book_stage);
            joiner.threads[0] = std::thread(run_stage, std::size_t{0}, parser_stage);
        } catch (...) {
            // A thread could not be created. Release the ones that were, or
            // joining them would wait for input that is never coming.
            events.cancel();
            updates.cancel();
            throw;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(end - start).count();

    for (const std::exception_ptr& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    result.counters = manager->counters();
    result.stats = manager->stats();
    result.audit = manager->audit();
    result.event_channel = events.stats();
    result.update_channel = updates.stats();
    return result;
}

}  // namespace obe::pipeline
