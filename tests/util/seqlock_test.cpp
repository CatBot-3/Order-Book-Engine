#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "obe/book/types.hpp"
#include "obe/pipeline/top_of_book.hpp"
#include "obe/util/seqlock.hpp"
#include "support/printers.hpp"
#include "support/threads.hpp"

// Seqlock<T> and the top-of-book board built on it.
//
// The property that matters is that a reader never returns a value no writer
// ever stored: no mix of the first half of one update with the second half of
// the next. The two-thread tests store records whose fields are tied together
// and have readers check the ties on every load. Run under ThreadSanitizer
// they also show the implementation has no data race, which the textbook
// seqlock, with a plain value, does have.

namespace {

using namespace obe;

// Six words whose contents all follow from the first. Larger than a cache
// line would allow a single instruction to copy, so a torn read is possible
// in principle and must be ruled out by the sequence number.
struct Record {
    std::uint64_t n = 0;
    std::uint64_t twice = 0;
    std::uint64_t inverse = 0;
    std::array<std::uint64_t, 3> copies{};

    static Record make(std::uint64_t n) { return {n, n * 2, ~n, {n, n, n}}; }
    [[nodiscard]] bool consistent() const {
        return twice == n * 2 && inverse == ~n && copies[0] == n && copies[1] == n &&
               copies[2] == n;
    }
    friend bool operator==(const Record&, const Record&) = default;
};

// Thirteen bytes: more than one word and less than two.
struct Odd {
    std::array<char, 13> text{};
    friend bool operator==(const Odd&, const Odd&) = default;
};

TEST(Seqlock, StartsWithAValueInitializedT) {
    const util::Seqlock<Record> lock;
    EXPECT_EQ(lock.load(), Record{});
    EXPECT_EQ(lock.version(), 1U);
}

TEST(Seqlock, CanStartWithAGivenValue) {
    const util::Seqlock<Record> lock(Record::make(7));
    EXPECT_EQ(lock.load(), Record::make(7));
    EXPECT_EQ(lock.version(), 1U);
}

TEST(Seqlock, LoadReturnsTheLastValueStored) {
    util::Seqlock<Record> lock;
    for (std::uint64_t i = 1; i <= 100; ++i) {
        lock.store(Record::make(i));
        ASSERT_EQ(lock.load(), Record::make(i));
        ASSERT_EQ(lock.version(), i + 1);
    }
}

TEST(Seqlock, TryLoadSucceedsWhenNobodyIsWriting) {
    util::Seqlock<Record> lock;
    lock.store(Record::make(5));
    Record out;
    ASSERT_TRUE(lock.try_load(out));
    EXPECT_EQ(out, Record::make(5));
}

TEST(Seqlock, HandlesTypesThatAreNotAWholeNumberOfWords) {
    util::Seqlock<Odd> lock;
    Odd value;
    value.text = {'t', 'h', 'i', 'r', 't', 'e', 'e', 'n', ' ', 'c', 'h', 'r', 's'};
    lock.store(value);
    EXPECT_EQ(lock.load(), value);

    util::Seqlock<char> one;
    one.store('q');
    EXPECT_EQ(one.load(), 'q');

    util::Seqlock<std::uint64_t> word(std::uint64_t{0xdeadbeefcafef00dULL});
    EXPECT_EQ(word.load(), 0xdeadbeefcafef00dULL);
}

// How many loads each reader must have made before the writer may stop. The
// writer finishes its minimum number of stores in milliseconds; on a busy
// machine a reader thread may not even have started by then, and a test in
// which nobody was reading would prove nothing.
constexpr std::uint64_t kLoadsEach = 2'000;
// A bound, so that a reader that never makes progress fails the test instead
// of hanging it.
constexpr std::uint64_t kMostWrites = 2'000'000'000;

TEST(Seqlock, ReadersNeverSeeAValueNobodyStored) {
    constexpr std::uint64_t kWrites = 300'000;
    constexpr std::size_t kReaders = 3;
    // Not a default Record: all zeros does not satisfy the ties (the inverse
    // of 0 is not 0), and a reader that ran before the first store would
    // report it as torn.
    util::Seqlock<Record> lock(Record::make(0));
    std::atomic<bool> done{false};
    std::array<std::atomic<std::uint64_t>, kReaders> loads{};
    std::array<std::uint64_t, kReaders> torn{};
    std::array<std::uint64_t, kReaders> backwards{};

    test::Abort abort;
    std::uint64_t written = 0;
    {
        std::vector<std::unique_ptr<test::Worker>> readers;
        for (std::size_t r = 0; r < kReaders; ++r) {
            readers.push_back(std::make_unique<test::Worker>(abort, [&, r] {
                std::uint64_t last = 0;
                while (!done.load(std::memory_order_acquire)) {
                    const Record seen = lock.load();
                    loads[r].fetch_add(1, std::memory_order_relaxed);
                    if (!seen.consistent()) {
                        ++torn[r];
                    }
                    // One writer counting upwards: a reader can skip values
                    // but can never go back to an older one.
                    if (seen.n < last) {
                        ++backwards[r];
                    }
                    last = seen.n;
                }
            }));
        }
        const auto readers_busy = [&] {
            for (const std::atomic<std::uint64_t>& n : loads) {
                if (n.load(std::memory_order_relaxed) < kLoadsEach) {
                    return false;
                }
            }
            return true;
        };
        while (written < kMostWrites && (written < kWrites || !readers_busy())) {
            lock.store(Record::make(++written));
        }
        done.store(true, std::memory_order_release);
        for (auto& reader : readers) {
            reader->join();
        }
    }

    for (std::size_t r = 0; r < kReaders; ++r) {
        EXPECT_EQ(torn[r], 0U) << "reader " << r << " saw a mix of two values";
        EXPECT_EQ(backwards[r], 0U) << "reader " << r << " went back in time";
        EXPECT_GE(loads[r].load(), kLoadsEach) << "reader " << r << " never got going";
    }
    EXPECT_EQ(lock.load(), Record::make(written));
    EXPECT_EQ(lock.version(), written + 1);
    EXPECT_FALSE(Record{}.consistent()) << "the check must be able to fail";
}

TEST(Seqlock, TryLoadEitherFailsOrReturnsAWholeValue) {
    constexpr std::uint64_t kWrites = 200'000;
    util::Seqlock<Record> lock(Record::make(0));
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> ok{0};
    std::uint64_t torn = 0;

    test::Abort abort;
    test::Worker reader(abort, [&] {
        Record seen;
        while (!done.load(std::memory_order_acquire)) {
            if (lock.try_load(seen)) {
                ok.fetch_add(1, std::memory_order_relaxed);
                if (!seen.consistent()) {
                    ++torn;
                }
            }
        }
    });
    std::uint64_t written = 0;
    while (written < kMostWrites &&
           (written < kWrites || ok.load(std::memory_order_relaxed) < kLoadsEach)) {
        lock.store(Record::make(++written));
    }
    done.store(true, std::memory_order_release);
    reader.join();
    EXPECT_EQ(torn, 0U);
    EXPECT_GE(ok.load(), kLoadsEach) << "the reader never managed a load between writes";
}

// --- The board ---------------------------------------------------------------

book::BboUpdate update(Locate locate, Nanos t, Price bid, Price ask) {
    return {.locate = locate,
            .timestamp = t,
            .bbo = {.bid_price = bid, .bid_qty = 100, .ask_price = ask, .ask_qty = 200}};
}

TEST(TopOfBookBoard, ALocateNobodyPublishedReadsAsEmpty) {
    const pipeline::TopOfBookBoard board;
    EXPECT_EQ(board.read(0), book::BboUpdate{});
    EXPECT_EQ(board.read(65'535), book::BboUpdate{});
    EXPECT_EQ(board.updates(7), 0U);
}

TEST(TopOfBookBoard, KeepsTheLatestUpdateOfEachLocateSeparately) {
    pipeline::TopOfBookBoard board;
    board.publish(update(7, 10, 1'000'000, 1'000'100));
    board.publish(update(8, 11, 2'000'000, 2'000'100));
    board.publish(update(7, 12, 1'000'050, 1'000'100));
    EXPECT_EQ(board.read(7), update(7, 12, 1'000'050, 1'000'100));
    EXPECT_EQ(board.read(8), update(8, 11, 2'000'000, 2'000'100));
    EXPECT_EQ(board.updates(7), 2U);
    EXPECT_EQ(board.updates(8), 1U);
    EXPECT_EQ(board.updates(9), 0U);

    book::BboUpdate out;
    ASSERT_TRUE(board.try_read(65'535, out));
    EXPECT_EQ(out, book::BboUpdate{});
}

TEST(TopOfBookBoard, AReaderOnAnotherThreadSeesWholeUpdatesInOrder) {
    constexpr std::uint64_t kUpdates = 200'000;
    pipeline::TopOfBookBoard board;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> reads{0};
    std::uint64_t torn = 0;
    std::uint64_t backwards = 0;

    test::Abort abort;
    test::Worker reader(abort, [&] {
        Nanos last = 0;
        while (!done.load(std::memory_order_acquire)) {
            const book::BboUpdate seen = board.read(3);
            reads.fetch_add(1, std::memory_order_relaxed);
            // The writer below ties the prices to the timestamp: its low 32
            // bits, which is all a Price holds.
            const auto price = static_cast<Price>(seen.timestamp);
            if (seen.timestamp != 0 && (seen.locate != 3 || seen.bbo.bid_price != price ||
                                        seen.bbo.ask_price != price + 1)) {
                ++torn;
            }
            if (seen.timestamp < last) {
                ++backwards;
            }
            last = seen.timestamp;
        }
    });
    // Until the minimum is written and the reader has really been reading.
    std::uint64_t written = 0;
    while (written < kMostWrites &&
           (written < kUpdates || reads.load(std::memory_order_relaxed) < kLoadsEach)) {
        ++written;
        const auto price = static_cast<Price>(written);
        board.publish(update(3, written, price, price + 1));
        // Its neighbours are written too: a slot must not disturb the next.
        board.publish(update(2, written, 1, 2));
        board.publish(update(4, written, 3, 4));
    }
    done.store(true, std::memory_order_release);
    reader.join();
    EXPECT_EQ(torn, 0U);
    EXPECT_EQ(backwards, 0U);
    EXPECT_GE(reads.load(), kLoadsEach);
    EXPECT_EQ(board.updates(3), written);
}

}  // namespace
