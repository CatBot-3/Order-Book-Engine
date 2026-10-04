// queue_bench: one thread pushes a counted stream through a queue, another
// pops it and checks the order.
//
//   queue_bench [options]
//     --queue NAME       mutex | mutex-spin | ring (default: mutex);
//                        --list prints them
//     --items N          items per run (default 20000000)
//     --capacity N       queue capacity (default 65536)
//     --payload N        bytes per item: 8 or 40 (default 40, the size of the
//                        pipeline's BookEvent)
//     --runs N           measured runs (default 5)
//     --warmup N         unmeasured runs before them (default 1)
//     --cpus P,C         pin the producer to CPU P and the consumer to CPU C
//                        (default: not pinned)
//     --json FILE        also write the results as JSON
//     --label TEXT       free text carried into the report
//
// It is two things at once.
//
// A benchmark: the time to move N items from one thread to another, for the
// mutex queue and for the lock-free ring. That is the comparison phase 6 asks
// for. What is timed is the whole transfer, from the moment both threads are
// released to the moment the consumer has taken the last item.
//
// A stress test: the items are the integers 0, 1, 2, ... and the consumer
// checks every one. A queue that loses, repeats or reorders an item fails the
// run. With --items 1000000000 this is the ordered run of one billion items
// that success criterion 6 names; scripts/queue_stress.sh runs exactly that.
//
// Where the two threads run matters more here than in any other benchmark of
// this project. Two hyper-threads of one core, two cores of one socket and
// two unpinned threads the scheduler keeps moving give three different
// answers. Pin them with --cpus and say which CPUs they were.
//
// Exit status: 0 ok, 1 usage error, 3 an item arrived out of order (the
// timings are void), 4 the chosen queue is not written yet.

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "bench_env.hpp"
#include "obe/util/backoff.hpp"
#include "obe/util/channels.hpp"
#include "obe/util/cpu.hpp"
#include "obe/util/format.hpp"
#include "obe/util/huge_pages.hpp"
#include "obe/util/queue_concepts.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;
using bench::summarize;
using bench::Summary;

struct Options {
    std::string queue = "mutex";
    std::string json_path;
    std::string label;
    std::uint64_t items = 20'000'000;
    std::size_t capacity = std::size_t{1} << 16;
    std::size_t payload = 40;
    int runs = 5;
    int warmup = 1;
    int producer_cpu = -1;
    int consumer_cpu = -1;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: queue_bench [--queue NAME] [--items N] [--capacity N] [--payload 8|40]\n"
                 "                   [--runs N] [--warmup N] [--cpus P,C] [--json FILE] "
                 "[--label TEXT]\n"
                 "       queue_bench --list\n");
    return status;
}

void list_queues() {
    util::for_each_channel([]<class Kind>(std::type_identity<Kind>) {
        std::printf("%-11.*s %.*s\n", static_cast<int>(Kind::kName.size()), Kind::kName.data(),
                    static_cast<int>(Kind::kDescription.size()), Kind::kDescription.data());
    });
}

template <class T>
bool parse_number(std::string_view text, T& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

// The two payloads. Both carry the item's index; the larger one also carries
// values derived from it, so that the consumer can tell a record that was
// read before it was completely written.
struct Small {
    std::uint64_t index = 0;

    static Small make(std::uint64_t i) noexcept { return {i}; }
    [[nodiscard]] bool consistent() const noexcept { return true; }
};

struct EventSized {
    std::uint64_t index = 0;
    std::uint64_t twice = 0;
    std::uint64_t inverse = 0;
    std::uint64_t again = 0;
    std::uint64_t last = 0;

    static EventSized make(std::uint64_t i) noexcept { return {i, i * 2, ~i, i, i}; }
    [[nodiscard]] bool consistent() const noexcept {
        return twice == index * 2 && inverse == ~index && again == index && last == index;
    }
};

static_assert(sizeof(Small) == 8);
static_assert(sizeof(EventSized) == 40);

struct Run {
    double seconds = 0;
    double items_per_s = 0;
    double ns_per_item = 0;
    std::uint64_t received = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t inconsistent = 0;
    util::ChannelStats waits;
    bool producer_pinned = false;
    bool consumer_pinned = false;
};

template <class Kind, class Item>
Run one_run(const Options& opt) {
    using Channel = typename Kind::template Channel<Item>;
    Channel channel(opt.capacity);
    Run run;
    std::exception_ptr producer_error;
    std::exception_ptr consumer_error;
    // Both threads wait here until the clock is about to start, so that
    // creating them is not part of the measurement.
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    const auto wait_for_go = [&] {
        ready.fetch_add(1, std::memory_order_release);
        util::Backoff backoff;
        while (!go.load(std::memory_order_acquire)) {
            backoff.pause();
        }
    };

    std::thread consumer([&] {
        try {
            if (opt.consumer_cpu >= 0) {
                run.consumer_pinned = util::pin_to_cpu(opt.consumer_cpu);
            }
            wait_for_go();
            std::uint64_t expected = 0;
            std::uint64_t out_of_order = 0;
            std::uint64_t inconsistent = 0;
            Item item;
            while (channel.pop(item)) {
                out_of_order += item.index != expected ? 1U : 0U;
                inconsistent += item.consistent() ? 0U : 1U;
                ++expected;
            }
            run.received = expected;
            run.out_of_order = out_of_order;
            run.inconsistent = inconsistent;
        } catch (...) {
            consumer_error = std::current_exception();
            channel.cancel();
        }
    });
    std::thread producer([&] {
        try {
            if (opt.producer_cpu >= 0) {
                run.producer_pinned = util::pin_to_cpu(opt.producer_cpu);
            }
            wait_for_go();
            for (std::uint64_t i = 0; i < opt.items; ++i) {
                if (!channel.push(Item::make(i))) {
                    return;
                }
            }
            channel.close();
        } catch (...) {
            producer_error = std::current_exception();
            channel.cancel();
        }
    });

    {
        util::Backoff backoff;
        while (ready.load(std::memory_order_acquire) != 2) {
            backoff.pause();
        }
    }
    const auto start = std::chrono::steady_clock::now();
    // ---- timed region starts ----
    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    // ---- timed region ends ----
    const auto end = std::chrono::steady_clock::now();

    if (producer_error) {
        std::rethrow_exception(producer_error);
    }
    if (consumer_error) {
        std::rethrow_exception(consumer_error);
    }
    run.seconds = std::chrono::duration<double>(end - start).count();
    if (run.seconds > 0 && run.received > 0) {
        run.items_per_s = static_cast<double>(run.received) / run.seconds;
        run.ns_per_item = run.seconds * 1e9 / static_cast<double>(run.received);
    }
    run.waits = channel.stats();
    return run;
}

struct Report {
    Options options;
    std::string description;
    bench::Environment env;
    std::size_t actual_capacity = 0;
    std::size_t huge_bytes = 0;
    std::vector<Run> runs;
    std::vector<std::string> warnings;

    [[nodiscard]] bool in_order() const {
        for (const Run& r : runs) {
            if (r.out_of_order != 0 || r.inconsistent != 0 || r.received != options.items) {
                return false;
            }
        }
        return true;
    }
};

void print_text(const Report& r) {
    const Summary rate = summarize(r.runs, [](const Run& x) { return x.items_per_s; });
    const Summary ns = summarize(r.runs, [](const Run& x) { return x.ns_per_item; });

    std::printf("queue_bench%s%s\n",
                r.options.label.empty() ? "" : "  label: ", r.options.label.c_str());
    std::printf("  queue       %s: %s\n", r.options.queue.c_str(), r.description.c_str());
    std::printf("  transfer    %s items of %zu bytes through a queue of %s slots\n",
                util::with_commas(r.options.items).c_str(), r.options.payload,
                util::with_commas(r.actual_capacity).c_str());
    std::printf("  runs        %d measured after %d warm-up\n", r.options.runs, r.options.warmup);
    bench::print_environment(r.env, r.huge_bytes);
    if (r.options.producer_cpu >= 0 || r.options.consumer_cpu >= 0) {
        std::printf("  threads     producer on CPU %d%s, consumer on CPU %d%s\n",
                    r.options.producer_cpu,
                    !r.runs.empty() && r.runs.front().producer_pinned ? "" : " (NOT pinned)",
                    r.options.consumer_cpu,
                    !r.runs.empty() && r.runs.front().consumer_pinned ? "" : " (NOT pinned)");
    } else {
        std::printf("  threads     producer and consumer NOT pinned\n");
    }
    std::printf("  order       %s\n", r.in_order() ? "every item arrived once, in order, intact"
                                                   : "ITEMS LOST, REPEATED, REORDERED OR TORN: THE "
                                                     "TIMINGS BELOW ARE VOID");

    std::printf("\n  %-6s %14s %10s %14s %14s\n", "run", "items/s", "ns/item", "push waits",
                "pop waits");
    for (std::size_t i = 0; i < r.runs.size(); ++i) {
        const Run& x = r.runs[i];
        std::printf("  %-6zu %14.0f %10.2f %14s %14s\n", i + 1, x.items_per_s, x.ns_per_item,
                    util::with_commas(x.waits.push_waits).c_str(),
                    util::with_commas(x.waits.pop_waits).c_str());
        if (x.out_of_order != 0 || x.inconsistent != 0 || x.received != r.options.items) {
            std::printf("         received %s, out of order %s, torn %s\n",
                        util::with_commas(x.received).c_str(),
                        util::with_commas(x.out_of_order).c_str(),
                        util::with_commas(x.inconsistent).c_str());
        }
    }
    std::printf("  %-6s %14.0f %10.2f\n", "median", rate.median, ns.median);
    std::printf("  %-6s %14.0f %10.2f\n", "min", rate.min, ns.min);
    std::printf("  %-6s %14.0f %10.2f\n", "max", rate.max, ns.max);
    std::printf("\n  spread across runs: %.1f%% of the median\n", rate.spread_percent());
    std::printf("  a push wait is a push that found the queue full; a pop wait is a pop that\n");
    std::printf("  found it empty. Many push waits mean the consumer is the slower side.\n");
    std::printf("  ns/item is elapsed time over items: two cores were busy for that long.\n");

    for (const std::string& w : r.warnings) {
        std::printf("\n  WARNING: %s\n", w.c_str());
    }
}

bool write_json(const Report& r) {
    std::FILE* f = std::fopen(r.options.json_path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    const Summary rate = summarize(r.runs, [](const Run& x) { return x.items_per_s; });
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"benchmark\": \"queue_bench\",\n");
    std::fprintf(f, "  \"label\": \"%s\",\n", bench::json_escape(r.options.label).c_str());
    std::fprintf(f, "  \"workload\": \"queue/%s\",\n", bench::json_escape(r.options.queue).c_str());
    std::fprintf(f, "  \"messages\": %" PRIu64 ",\n", r.options.items);
    std::fprintf(f, "  \"capacity\": %zu,\n", r.actual_capacity);
    std::fprintf(f, "  \"payload_bytes\": %zu,\n", r.options.payload);
    std::fprintf(f, "  \"producer_cpu\": %d,\n", r.options.producer_cpu);
    std::fprintf(f, "  \"consumer_cpu\": %d,\n", r.options.consumer_cpu);
    std::fprintf(f, "  \"warmup_runs\": %d,\n", r.options.warmup);
    bench::write_environment_json(f, r.env, r.huge_bytes);
    std::fprintf(f, "  \"in_order\": %s,\n", r.in_order() ? "true" : "false");
    std::fprintf(f, "  \"median_msgs_per_s\": %.0f,\n", rate.median);
    std::fprintf(f, "  \"min_msgs_per_s\": %.0f,\n", rate.min);
    std::fprintf(f, "  \"max_msgs_per_s\": %.0f,\n", rate.max);
    std::fprintf(f, "  \"runs\": [\n");
    for (std::size_t i = 0; i < r.runs.size(); ++i) {
        const Run& x = r.runs[i];
        std::fprintf(f,
                     "    {\"msgs_per_s\": %.0f, \"mean_ns\": %.3f, \"push_waits\": %zu, "
                     "\"pop_waits\": %zu, \"received\": %" PRIu64 ", \"out_of_order\": %" PRIu64
                     "}%s\n",
                     x.items_per_s, x.ns_per_item, x.waits.push_waits, x.waits.pop_waits,
                     x.received, x.out_of_order, i + 1 == r.runs.size() ? "" : ",");
    }
    std::fprintf(f, "  ],\n");
    std::fprintf(f, "  \"warnings\": [");
    for (std::size_t i = 0; i < r.warnings.size(); ++i) {
        std::fprintf(f, "%s\"%s\"", i == 0 ? "" : ", ", bench::json_escape(r.warnings[i]).c_str());
    }
    std::fprintf(f, "]\n}\n");
    return std::fclose(f) == 0;
}

template <class Kind, class Item>
int measure(const Options& opt, Report& report) {
    report.description = std::string(Kind::kDescription);
    {
        const typename Kind::template Channel<Item> probe(opt.capacity);
        report.actual_capacity = probe.capacity();
    }
    for (int i = 0; i < opt.warmup; ++i) {
        static_cast<void>(one_run<Kind, Item>(opt));
    }
    for (int i = 0; i < opt.runs; ++i) {
        report.runs.push_back(one_run<Kind, Item>(opt));
    }
    report.huge_bytes = util::process_huge_bytes();

    const Summary rate = summarize(report.runs, [](const Run& x) { return x.items_per_s; });
    if (rate.spread_percent() > 5.0) {
        report.warnings.emplace_back(
            "throughput varied by more than 5% across runs: fix the environment before "
            "comparing two queues");
    }
    print_text(report);
    if (!opt.json_path.empty() && !write_json(report)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", opt.json_path.c_str());
        return 1;
    }
    return report.in_order() ? 0 : 3;
}

bool parse_cpus(std::string_view text, int& producer, int& consumer) {
    const std::size_t comma = text.find(',');
    return comma != std::string_view::npos && parse_number(text.substr(0, comma), producer) &&
           parse_number(text.substr(comma + 1), consumer) && producer >= 0 && consumer >= 0;
}

int run(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = i + 1 < argc;
        std::uint64_t value = 0;
        const bool has_value = has_next && parse_number(std::string_view(argv[i + 1]), value);
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--list") {
            list_queues();
            return 0;
        }
        if (arg == "--queue" && has_next) {
            opt.queue = argv[++i];
        } else if (arg == "--json" && has_next) {
            opt.json_path = argv[++i];
        } else if (arg == "--label" && has_next) {
            opt.label = argv[++i];
        } else if (arg == "--cpus" && has_next) {
            if (!parse_cpus(argv[++i], opt.producer_cpu, opt.consumer_cpu)) {
                std::fprintf(stderr, "error: --cpus needs two CPU numbers, as in --cpus 2,4\n");
                return 1;
            }
        } else if (arg == "--items" && has_value) {
            opt.items = value;
            ++i;
        } else if (arg == "--capacity" && has_value) {
            opt.capacity = static_cast<std::size_t>(value);
            ++i;
        } else if (arg == "--payload" && has_value) {
            opt.payload = static_cast<std::size_t>(value);
            ++i;
        } else if (arg == "--runs" && has_value) {
            opt.runs = static_cast<int>(value);
            ++i;
        } else if (arg == "--warmup" && has_value) {
            opt.warmup = static_cast<int>(value);
            ++i;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (opt.payload != 8 && opt.payload != 40) {
        std::fprintf(stderr, "error: --payload must be 8 or 40\n");
        return 1;
    }
    if (opt.runs < 1 || opt.items == 0 || opt.capacity == 0) {
        std::fprintf(stderr, "error: --runs, --items and --capacity must be at least 1\n");
        return 1;
    }

    Report report;
    report.options = opt;
    report.env = bench::gather_environment(opt.producer_cpu, opt.producer_cpu >= 0);
    if (opt.producer_cpu < 0) {
        report.warnings.emplace_back(
            "the threads are not pinned (--cpus P,C): where the scheduler puts them "
            "decides the result");
    }
    if (report.env.logical_cpus < 2) {
        report.warnings.emplace_back(
            "this machine has one CPU: the two threads take turns and the numbers mean "
            "nothing");
    }
    if (!bench::is_release_build() || bench::is_instrumented_build()) {
        report.warnings.emplace_back(
            "this is not an optimized, uninstrumented build: the timings mean nothing");
    }
    if (opt.runs < 5) {
        report.warnings.emplace_back("fewer than 5 measured runs: not enough to quote");
    }

    int status = 1;
    const bool known = util::with_channel(opt.queue, [&]<class Kind>(std::type_identity<Kind>) {
        status = opt.payload == 8 ? measure<Kind, Small>(opt, report)
                                  : measure<Kind, EventSized>(opt, report);
    });
    if (!known) {
        std::fprintf(stderr, "error: no queue called '%s'. Try --list.\n", opt.queue.c_str());
        return 1;
    }
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const obe::util::Unimplemented& e) {
        std::fprintf(stderr, "not built yet: %s\n", e.what());
        return 4;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
