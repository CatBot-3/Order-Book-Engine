#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

#include "obe/net/open_loop.hpp"
#include "obe/util/latency_histogram.hpp"

// The schedule and the acknowledgement matching that load_gen is built on.
//
// load_gen's claim is that a stall in the server shows in its percentiles at
// its true size. A claim about measurement is the kind that is easy to make
// and easy to get wrong without noticing, because a generator that hides
// stalls produces better-looking numbers, not an error. So the last tests here
// put the two ways of generating load side by side against a server that
// stalls on purpose, in simulated time, where the right answer can be worked
// out by hand.

namespace {

using namespace obe;

constexpr std::uint64_t kSecond = 1'000'000'000ULL;
constexpr std::uint64_t kMilli = 1'000'000ULL;
constexpr std::uint64_t kMicro = 1'000ULL;

// --- The schedule -------------------------------------------------------------

// Usable in a constant expression: there is no state behind it but two numbers.
static_assert(net::OpenLoopSchedule(5, 4).due(0) == 5);
static_assert(net::OpenLoopSchedule(5, 4).due(4) == 5 + kSecond);

TEST(OpenLoopSchedule, TheFirstRequestIsDueAtTheStart) {
    const net::OpenLoopSchedule schedule(123'456, 1'000);
    EXPECT_EQ(schedule.due(0), 123'456U);
    EXPECT_EQ(schedule.rate(), 1'000U);
}

TEST(OpenLoopSchedule, ARateThatDividesASecondIsEvenlySpaced) {
    const net::OpenLoopSchedule schedule(0, 1'000);
    for (std::uint64_t i = 0; i < 5'000; ++i) {
        ASSERT_EQ(schedule.due(i), i * kMilli);
    }
}

TEST(OpenLoopSchedule, AfterEveryWholeSecondExactlyRateRequestsHaveFallenDue) {
    // None of these divides a second. Whatever the rounding inside a second,
    // request number k * rate must be due exactly k seconds in: that is what
    // "this many per second" means over a long run.
    for (const std::uint64_t rate : {std::uint64_t{3}, std::uint64_t{7}, std::uint64_t{30'000},
                                     std::uint64_t{999'983}, std::uint64_t{1'500'000}}) {
        const net::OpenLoopSchedule schedule(42, rate);
        for (std::uint64_t k = 0; k <= 1'000; ++k) {
            ASSERT_EQ(schedule.due(k * rate), 42 + k * kSecond)
                << "rate " << rate << " second " << k;
        }
    }
}

TEST(OpenLoopSchedule, AddingARoundedIntervalWouldHaveDrifted) {
    // What the obvious implementation does: next = previous + 1e9 / rate, in
    // whole nanoseconds. At 30,000 a second the interval is 33,333.33 ns and
    // the third of a nanosecond thrown away each time adds up.
    constexpr std::uint64_t kRate = 30'000;
    constexpr std::uint64_t kHour = 3'600;
    const std::uint64_t interval = kSecond / kRate;
    const std::uint64_t naive = interval * kRate * kHour;  // when the hour's last request is due
    const net::OpenLoopSchedule schedule(0, kRate);
    EXPECT_EQ(schedule.due(kRate * kHour), kHour * kSecond);
    // 36 milliseconds early after an hour: the rate offered was not the rate
    // asked for, and every due time after the first second is wrong.
    EXPECT_EQ(kHour * kSecond - naive, 36 * kMilli);
}

TEST(OpenLoopSchedule, GapsDifferByAtMostANanosecond) {
    const net::OpenLoopSchedule schedule(0, 7);  // 142,857,142.86 ns apart
    for (std::uint64_t i = 0; i < 100; ++i) {
        const std::uint64_t gap = schedule.due(i + 1) - schedule.due(i);
        ASSERT_GE(gap, kSecond / 7);
        ASSERT_LE(gap, kSecond / 7 + 1);
    }
}

TEST(OpenLoopSchedule, NeverGoesBackwardsEvenAboveOneRequestPerNanosecond) {
    // Several requests then share a nanosecond, which is fine; what must not
    // happen is a later request being due before an earlier one.
    constexpr std::uint64_t kRate = 3'000'000'000ULL;
    const net::OpenLoopSchedule schedule(0, kRate);
    std::uint64_t last = 0;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        const std::uint64_t due = schedule.due(i);
        ASSERT_GE(due, last);
        last = due;
    }
    EXPECT_EQ(schedule.due(kRate), kSecond);
    EXPECT_EQ(schedule.due(kRate - 1), kSecond - 1);
}

TEST(OpenLoopSchedule, DoesNotOverflowOnALongRun) {
    // A year at ten million a second. The request's number times a billion
    // does not fit in 64 bits, so "i * 1e9 / rate" would have wrapped round
    // long before this.
    constexpr std::uint64_t kRate = 10'000'000;
    constexpr std::uint64_t kYear = 365ULL * 24 * 3'600;
    const net::OpenLoopSchedule schedule(1, kRate);
    EXPECT_EQ(schedule.due(kRate * kYear), 1 + kYear * kSecond);
    EXPECT_EQ(schedule.due(kRate * kYear + 1), 1 + kYear * kSecond + 100);
}

TEST(OpenLoopSchedule, ARateOfZeroIsTreatedAsOne) {
    // There is no sensible schedule for "no requests per second", and a
    // division by zero is not a useful way of saying so.
    const net::OpenLoopSchedule schedule(0, 0);
    EXPECT_EQ(schedule.rate(), 1U);
    EXPECT_EQ(schedule.due(3), 3 * kSecond);
}

// --- Matching acknowledgements -------------------------------------------------

TEST(AckQueue, StartsEmpty) {
    const net::AckQueue queue;
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.waiting(), 0U);
}

TEST(AckQueue, AnAcknowledgementBelongsToTheOldestRequest) {
    net::AckQueue queue;
    queue.sent(100);
    queue.sent(200);
    queue.sent(300);
    EXPECT_EQ(queue.waiting(), 3U);
    EXPECT_EQ(queue.oldest(), 100U);

    EXPECT_EQ(queue.acknowledged(1'000), 900U);
    EXPECT_EQ(queue.oldest(), 200U);
    // More requests go out while earlier ones are still unanswered.
    queue.sent(400);
    EXPECT_EQ(queue.acknowledged(1'000), 800U);
    EXPECT_EQ(queue.acknowledged(1'050), 750U);
    EXPECT_EQ(queue.acknowledged(1'100), 700U);
    EXPECT_TRUE(queue.empty());
}

TEST(AckQueue, ARequestWrittenLateIsChargedFromWhenItWasDue) {
    // Due at 1,000. The generator was busy and wrote it at 5,000; the server
    // answered 200 later. Whoever wanted that order placed at 1,000 waited
    // 4,200, and that is what is recorded. The queue is never told when the
    // request was written, so it could not record 200 if it wanted to.
    net::AckQueue queue;
    queue.sent(1'000);
    EXPECT_EQ(queue.acknowledged(5'200), 4'200U);
}

TEST(AckQueue, AnAnswerBeforeTheDueTimeCountsAsZero) {
    // Cannot happen with one clock, since a request is not written before it
    // is due. It is still not allowed to wrap round to eighteen quintillion
    // nanoseconds and become the maximum of a report.
    net::AckQueue queue;
    queue.sent(1'000);
    EXPECT_EQ(queue.acknowledged(999), 0U);
}

// --- The claim: a stall is not hidden -------------------------------------------

// A server that answers one request at a time, each taking `service`, and does
// nothing at all between `stall_from` and `stall_until`.
class StallingServer {
 public:
    StallingServer(std::uint64_t service, std::uint64_t stall_from, std::uint64_t stall_until)
        : service_(service), stall_from_(stall_from), stall_until_(stall_until) {}

    // A request arrives at `arrives`; returns when its answer is ready.
    // Requests must be given in order of arrival.
    std::uint64_t answer(std::uint64_t arrives) {
        std::uint64_t begins = std::max(arrives, free_at_);
        if (begins >= stall_from_ && begins < stall_until_) {
            begins = stall_until_;
        }
        free_at_ = begins + service_;
        return free_at_;
    }

 private:
    std::uint64_t service_;
    std::uint64_t stall_from_;
    std::uint64_t stall_until_;
    std::uint64_t free_at_ = 0;
};

// The experiment: 1,000 requests a second for ten seconds, a server that takes
// ten microseconds a request, and one stall of a whole second in the middle.
constexpr std::uint64_t kRate = 1'000;
constexpr std::uint64_t kRunSeconds = 10;
constexpr std::uint64_t kService = 10 * kMicro;
constexpr std::uint64_t kStallFrom = 4 * kSecond;
constexpr std::uint64_t kStallUntil = 5 * kSecond;
constexpr std::uint64_t kSlow = 100 * kMilli;  // what a user would call "it hung"

struct Outcome {
    util::LatencyHistogram latency;
    std::uint64_t slow = 0;  // samples of kSlow or more
    std::uint64_t waited_total = 0;
};

void record(Outcome& outcome, std::uint64_t took) {
    outcome.latency.record(took);
    outcome.slow += took >= kSlow ? 1U : 0U;
    outcome.waited_total += took;
}

// load_gen's way: every request goes out when it is due, whatever the server
// is doing, and is timed from when it was due.
Outcome open_loop() {
    StallingServer server(kService, kStallFrom, kStallUntil);
    const net::OpenLoopSchedule schedule(0, kRate);
    net::AckQueue queue;
    Outcome outcome;
    for (std::uint64_t i = 0; i < kRate * kRunSeconds; ++i) {
        const std::uint64_t due = schedule.due(i);
        queue.sent(due);
        record(outcome, queue.acknowledged(server.answer(due)));
    }
    return outcome;
}

// The common way: send, wait for the answer, then send the next one no sooner
// than one interval after the last, and time each from when it was sent.
Outcome closed_loop() {
    StallingServer server(kService, kStallFrom, kStallUntil);
    const std::uint64_t interval = kSecond / kRate;
    Outcome outcome;
    std::uint64_t send = 0;
    while (send < kRunSeconds * kSecond) {
        const std::uint64_t answered = server.answer(send);
        record(outcome, answered - send);
        send = std::max(answered, send + interval);
    }
    return outcome;
}

TEST(CoordinatedOmission, TheOpenLoopSendsEveryRequestTheScheduleCallsFor) {
    EXPECT_EQ(open_loop().latency.count(), kRate * kRunSeconds);
}

TEST(CoordinatedOmission, TheClosedLoopSilentlySendsFewerRequests) {
    // It sent nothing during the stall, so about a second's worth of requests
    // is missing from its samples, and nothing in its report says so.
    const std::uint64_t sent = closed_loop().latency.count();
    EXPECT_LT(sent, kRate * kRunSeconds - 900);
    EXPECT_GT(sent, kRate * kRunSeconds - 1'100);
}

TEST(CoordinatedOmission, TheOpenLoopChargesEveryRequestDueDuringTheStall) {
    const Outcome open = open_loop();
    // A thousand requests fell due while the server was stalled. Those due in
    // its first nine tenths waited at least a tenth of a second.
    EXPECT_GE(open.slow, 900U);
    EXPECT_LE(open.slow, 1'000U);
    // Together they waited the area of a triangle: a thousand requests, the
    // first waiting a second and the last almost nothing, about 500 seconds.
    EXPECT_GE(open.waited_total, 500 * kSecond);
    EXPECT_LE(open.waited_total, 520 * kSecond);
    // One request in ten was hit, so the 99th percentile is deep inside the
    // stall. (The histogram's buckets are a few percent wide.)
    EXPECT_GE(open.latency.percentile(99.0), 850 * kMilli);
    EXPECT_GE(open.latency.max(), kSecond);
}

TEST(CoordinatedOmission, TheClosedLoopRecordsTheSameStallAsOneBadSample) {
    const Outcome closed = closed_loop();
    EXPECT_EQ(closed.slow, 1U);
    EXPECT_GE(closed.latency.max(), kSecond);
    // One sample in nine thousand: every percentile anybody quotes is the
    // service time, as if the stall had not happened.
    EXPECT_LE(closed.latency.percentile(99.0), 2 * kService);
    EXPECT_LE(closed.latency.percentile(99.9), 2 * kService);
    // About one second of waiting recorded, where the open loop found 500.
    EXPECT_LE(closed.waited_total, 2 * kSecond);
}

TEST(CoordinatedOmission, WithoutAStallTheTwoAgree) {
    // The difference is only in how a slow server is seen, not in what a
    // healthy one looks like.
    StallingServer steady(kService, 0, 0);
    const net::OpenLoopSchedule schedule(0, kRate);
    net::AckQueue queue;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        const std::uint64_t due = schedule.due(i);
        queue.sent(due);
        ASSERT_EQ(queue.acknowledged(steady.answer(due)), kService);
    }
}

}  // namespace
