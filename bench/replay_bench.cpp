// replay_bench: time the processing of an ITCH file that is already in memory.
//
//   replay_bench <file> [options]
//     --handler book|parse   what is timed (default: book)
//                              book   framing + decode + reference book update
//                              parse  framing + decode only
//     --runs N               measured runs (default 5; the spec's minimum)
//     --warmup N             unmeasured runs before them (default 1)
//     --cpu N                pin the thread to CPU N (default: not pinned)
//     --clock tsc|steady     per-message clock (default: tsc where available)
//     --no-copy              time against the page cache instead of a private copy
//     --json FILE            also write the results as JSON
//     --label TEXT           free text carried into the report, e.g. "baseline"
//
// What is and is not inside the timed region is defined in
// docs/benchmark-method.md. Read that before quoting a number from here.
//
// Exit status: 0 ok, 1 usage or I/O error, 2 the file is truncated or corrupt,
// 3 the book's invariants were violated, 4 the book containers are not written
// yet (use --handler parse).

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "obe/book/bbo_hash.hpp"
#include "obe/book/book_manager.hpp"
#include "obe/book/order_store.hpp"
#include "obe/book/price_levels.hpp"
#include "obe/feed/parser.hpp"
#include "obe/util/clock.hpp"
#include "obe/util/cpu.hpp"
#include "obe/util/format.hpp"
#include "obe/util/latency_histogram.hpp"
#include "obe/util/mapped_file.hpp"
#include "obe/util/perf_counters.hpp"
#include "obe/util/todo.hpp"

#ifndef OBE_BUILD_FLAGS
#define OBE_BUILD_FLAGS "unknown"
#endif
#ifndef OBE_BUILD_TYPE
#define OBE_BUILD_TYPE "unknown"
#endif

namespace {

using namespace obe;
using util::LatencyHistogram;
using util::PerfCounters;

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    std::string path;
    std::string handler = "book";
    std::string clock = OBE_HAS_TSC ? "tsc" : "steady";
    std::string json_path;
    std::string label;
    int runs = 5;
    int warmup = 1;
    int cpu = -1;
    bool copy = true;
};

int usage(std::FILE* to, int status) {
    std::fprintf(to,
                 "usage: replay_bench <uncompressed ITCH 5.0 file> [--handler book|parse] "
                 "[--runs N] [--warmup N]\n"
                 "                    [--cpu N] [--clock tsc|steady] [--no-copy] [--json FILE] "
                 "[--label TEXT]\n");
    return status;
}

bool parse_int(std::string_view text, int& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

// ---------------------------------------------------------------------------
// The input, fully in memory before any clock starts
// ---------------------------------------------------------------------------

// A private, anonymous copy of the file. Unlike a file mapping, these pages
// cannot be dropped from the page cache and read back from disk in the middle
// of a timed run.
class MemoryCopy {
 public:
    explicit MemoryCopy(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            throw std::runtime_error("cannot open '" + path + "': " + std::strerror(errno));
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            throw std::runtime_error("'" + path + "' is not a readable regular file");
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ == 0) {
            ::close(fd);
            return;
        }
        void* p =
            ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            const int err = errno;
            ::close(fd);
            throw std::runtime_error("cannot allocate " + std::to_string(size_) +
                                     " bytes: " + std::strerror(err) + ". Try --no-copy.");
        }
        data_ = static_cast<std::byte*>(p);
        std::size_t done = 0;
        while (done < size_) {
            const ssize_t n = ::read(fd, data_ + done, size_ - done);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                const int err = errno;
                ::close(fd);
                ::munmap(data_, size_);
                data_ = nullptr;
                throw std::runtime_error("cannot read '" + path +
                                         "': " + (n == 0 ? "file shrank" : std::strerror(err)));
            }
            done += static_cast<std::size_t>(n);
        }
        ::close(fd);
    }
    MemoryCopy(const MemoryCopy&) = delete;
    MemoryCopy& operator=(const MemoryCopy&) = delete;
    ~MemoryCopy() {
        if (data_ != nullptr) {
            ::munmap(data_, size_);
        }
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

 private:
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

// Read one byte from every page so that a file mapping is resident before the
// clock starts. Returns a value that depends on what was read, so the loop
// cannot be optimized away.
std::uint64_t touch_pages(std::span<const std::byte> buf) {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < buf.size(); i += 4096) {
        sum += static_cast<std::uint64_t>(buf[i]);
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Workloads
// ---------------------------------------------------------------------------

// "parse": everything the feed layer does for a message, with nothing behind
// it. Each callback folds the fields a book would read into a checksum. That
// keeps the compiler from deleting the decode as dead code, and the checksum
// is printed so two runs can be seen to have done the same work.
struct ChecksumHandler : feed::HandlerBase {
    std::uint64_t sum = 0;

    void mix(std::uint64_t v) noexcept { sum = (sum ^ v) * 0x100000001b3ULL; }

    void on_add(const feed::AddOrder& m) noexcept {
        mix(m.order_ref);
        mix((static_cast<std::uint64_t>(m.price) << 32) | m.shares);
        mix(static_cast<std::uint64_t>(static_cast<unsigned char>(m.side)) ^ m.hdr.locate);
    }
    void on_execute(const feed::OrderExecuted& m) noexcept {
        mix(m.order_ref);
        mix(m.shares ^ m.hdr.locate);
    }
    void on_execute_with_price(const feed::OrderExecutedWithPrice& m) noexcept {
        mix(m.order_ref);
        mix((static_cast<std::uint64_t>(m.price) << 32) | m.shares);
        mix(static_cast<std::uint64_t>(static_cast<unsigned char>(m.printable)) ^ m.hdr.locate);
    }
    void on_cancel(const feed::OrderCancel& m) noexcept {
        mix(m.order_ref);
        mix(m.shares ^ m.hdr.locate);
    }
    void on_delete(const feed::OrderDelete& m) noexcept { mix(m.order_ref ^ m.hdr.locate); }
    void on_replace(const feed::OrderReplace& m) noexcept {
        mix(m.orig_order_ref);
        mix(m.new_order_ref);
        mix((static_cast<std::uint64_t>(m.price) << 32) | m.shares);
    }
    void on_stock_directory(const feed::StockDirectory& m) noexcept { mix(m.hdr.locate); }
    void on_trading_action(const feed::TradingAction& m) noexcept {
        mix(static_cast<std::uint64_t>(static_cast<unsigned char>(m.trading_state)));
    }
};

struct ParseWorkload {
    using Handler = ChecksumHandler;
    static constexpr const char* kName = "parse";
    static constexpr const char* kDescription =
        "framing + dispatch + decode into a checksum (no book)";

    static std::uint64_t digest(const Handler& h) { return h.sum; }
};

struct BookWorkload {
    using Handler = book::BookManager<book::OrderStore, book::PriceLevels>;
    static constexpr const char* kName = "book";
    static constexpr const char* kDescription =
        "framing + dispatch + decode + reference book (OrderStore, PriceLevels), no listener";

    // Depends on every message having been applied.
    static std::uint64_t digest(const Handler& h) {
        return (static_cast<std::uint64_t>(h.orders().size()) << 32) ^ h.stats().bbo_updates;
    }
};

// ---------------------------------------------------------------------------
// The timed passes
// ---------------------------------------------------------------------------

struct ThroughputPass {
    std::uint64_t messages = 0;
    double seconds = 0;
    std::uint64_t digest = 0;
    PerfCounters::Sample perf{};
    bool ok = false;
};

struct LatencyPass {
    LatencyHistogram ticks;
    bool ok = false;
};

// Pass 1: no per-message clock. Total time over total messages. This is the
// throughput figure, and the only pass the hardware counters are read around.
template <class Workload>
ThroughputPass throughput_pass(std::span<const std::byte> buf, PerfCounters& perf) {
    auto handler = std::make_unique<typename Workload::Handler>();
    feed::ItchParser parser(*handler);
    feed::FrameReader reader(buf);
    feed::Frame frame;
    ThroughputPass out;
    bool ok = true;

    perf.start();
    const auto start = std::chrono::steady_clock::now();
    // ---- timed region starts ----
    while (!reader.done()) {
        if (reader.next(frame) != feed::ParseStatus::Ok) [[unlikely]] {
            ok = false;
            break;
        }
        if (parser.dispatch(frame) != feed::ParseStatus::Ok) [[unlikely]] {
            ok = false;
            break;
        }
        ++out.messages;
    }
    // ---- timed region ends ----
    const auto end = std::chrono::steady_clock::now();
    out.perf = perf.stop();

    out.seconds = std::chrono::duration<double>(end - start).count();
    out.digest = Workload::digest(*handler);
    out.ok = ok;
    return out;
}

// Pass 2: one clock read per message. Each sample is the time from the
// previous read to this one, so it covers one message's framing, decode and
// handling, plus one clock read and one histogram update. The fixed part is
// measured separately by timer_floor() and reported next to the percentiles.
template <class Workload, class Clock>
std::unique_ptr<LatencyPass> latency_pass(std::span<const std::byte> buf) {
    auto handler = std::make_unique<typename Workload::Handler>();
    auto out = std::make_unique<LatencyPass>();
    feed::ItchParser parser(*handler);
    feed::FrameReader reader(buf);
    feed::Frame frame;
    LatencyHistogram& hist = out->ticks;
    bool ok = true;

    std::uint64_t previous = Clock::now();
    while (!reader.done()) {
        if (reader.next(frame) != feed::ParseStatus::Ok) [[unlikely]] {
            ok = false;
            break;
        }
        if (parser.dispatch(frame) != feed::ParseStatus::Ok) [[unlikely]] {
            ok = false;
            break;
        }
        const std::uint64_t now = Clock::now();
        hist.record(now - previous);
        previous = now;
    }
    out->ok = ok;
    return out;
}

// The latency loop with the message taken out: what a sample would read if
// processing took no time at all.
template <class Clock>
std::unique_ptr<LatencyHistogram> timer_floor(std::uint64_t samples) {
    auto hist = std::make_unique<LatencyHistogram>();
    std::uint64_t previous = Clock::now();
    for (std::uint64_t i = 0; i < samples; ++i) {
        const std::uint64_t now = Clock::now();
        hist->record(now - previous);
        previous = now;
    }
    return hist;
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

struct Run {
    std::uint64_t messages = 0;
    double seconds = 0;
    double msgs_per_s = 0;
    double mean_ns = 0;
    double p50_ns = 0;
    double p99_ns = 0;
    double p999_ns = 0;
    double max_ns = 0;
    std::uint64_t digest = 0;
    PerfCounters::Sample perf{};
};

struct Summary {
    double median = 0;
    double min = 0;
    double max = 0;
    // (max - min) / median, in percent.
    [[nodiscard]] double spread_percent() const {
        return median > 0 ? 100.0 * (max - min) / median : 0.0;
    }
};

template <class Get>
Summary summarize(const std::vector<Run>& runs, Get get) {
    std::vector<double> values;
    values.reserve(runs.size());
    for (const Run& r : runs) {
        values.push_back(get(r));
    }
    std::sort(values.begin(), values.end());
    Summary s;
    if (values.empty()) {
        return s;
    }
    const std::size_t n = values.size();
    s.median = n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
    s.min = values.front();
    s.max = values.back();
    return s;
}

struct Environment {
    std::string cpu_model;
    unsigned logical_cpus = 0;
    std::string memory;
    std::string kernel;
    std::string governor;
    std::string compiler;
    bool hypervisor = false;
    bool wsl = false;
    bool pinned = false;
    int cpu = -1;
};

std::string compiler_string() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

bool contains_ci(std::string haystack, std::string_view needle) {
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(needle) != std::string::npos;
}

Environment gather_environment(const Options& opt, bool pinned) {
    Environment env;
    env.cpu_model = util::read_proc_value("/proc/cpuinfo", "model name");
    env.logical_cpus = std::thread::hardware_concurrency();
    env.memory = util::read_proc_value("/proc/meminfo", "MemTotal");
    env.kernel = util::read_first_line("/proc/sys/kernel/osrelease");
    env.wsl = contains_ci(env.kernel, "microsoft");
    env.hypervisor =
        (" " + util::read_proc_value("/proc/cpuinfo", "flags") + " ").find(" hypervisor ") !=
        std::string::npos;
    const int cpu = pinned ? opt.cpu : 0;
    env.governor = util::read_first_line("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                                         "/cpufreq/scaling_governor");
    env.compiler = compiler_string();
    env.pinned = pinned;
    env.cpu = pinned ? opt.cpu : util::current_cpu();
    return env;
}

bool is_instrumented_build() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(undefined_behavior_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

bool is_release_build() {
#ifdef NDEBUG
    return true;
#else
    return false;
#endif
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Per-message value of a hardware counter in one run, or a negative number if
// that counter was not available.
double per_message(const Run& run, PerfCounters::Counter c) {
    if (!run.perf.valid[c] || run.messages == 0) {
        return -1.0;
    }
    return static_cast<double>(run.perf.value[c]) / static_cast<double>(run.messages);
}

struct Correctness {
    bool checked = false;
    bool clean = false;
    std::uint64_t hash = 0;
    book::Counters counters{};
    book::Audit audit{};
};

struct Report {
    Options options;
    Environment env;
    std::string workload_name;
    std::string workload_description;
    std::string clock_name;
    double ns_per_tick = 1.0;
    bool tsc_invariant = false;
    double floor_p50_ns = 0;
    double floor_p99_ns = 0;
    std::size_t file_bytes = 0;
    std::uint64_t messages = 0;
    bool perf_available = false;
    std::string perf_error;
    Correctness correctness;
    std::vector<Run> runs;
    std::vector<std::string> warnings;
};

void print_text(const Report& r) {
    const Summary rate = summarize(r.runs, [](const Run& x) { return x.msgs_per_s; });
    const Summary mean = summarize(r.runs, [](const Run& x) { return x.mean_ns; });
    const Summary p50 = summarize(r.runs, [](const Run& x) { return x.p50_ns; });
    const Summary p99 = summarize(r.runs, [](const Run& x) { return x.p99_ns; });
    const Summary p999 = summarize(r.runs, [](const Run& x) { return x.p999_ns; });
    const Summary max = summarize(r.runs, [](const Run& x) { return x.max_ns; });

    std::printf("replay_bench%s%s\n",
                r.options.label.empty() ? "" : "  label: ", r.options.label.c_str());
    std::printf("  file        %s\n", r.options.path.c_str());
    std::printf("              %s bytes, %s messages, %s\n",
                util::with_commas(r.file_bytes).c_str(), util::with_commas(r.messages).c_str(),
                r.options.copy ? "private in-memory copy" : "page cache (--no-copy)");
    std::printf("  workload    %s: %s\n", r.workload_name.c_str(), r.workload_description.c_str());
    std::printf("  runs        %d measured after %d warm-up\n", r.options.runs, r.options.warmup);
    std::printf("  cpu         %s\n", r.env.cpu_model.c_str());
    std::printf("              %u logical CPUs, %s RAM%s\n", r.env.logical_cpus,
                r.env.memory.c_str(), r.env.hypervisor ? ", running under a hypervisor" : "");
    if (r.env.pinned) {
        std::printf("  thread      pinned to CPU %d\n", r.env.cpu);
    } else {
        std::printf("  thread      NOT pinned (started on CPU %d)\n", r.env.cpu);
    }
    std::printf("  governor    %s\n", r.env.governor.empty() ? "unknown" : r.env.governor.c_str());
    std::printf("  kernel      %s%s\n", r.env.kernel.c_str(), r.env.wsl ? "  (WSL)" : "");
    std::printf("  compiler    %s\n", r.env.compiler.c_str());
    std::printf("  build       %s: %s\n", OBE_BUILD_TYPE, OBE_BUILD_FLAGS);
    std::printf("  clock       %s, %.4f ns per tick%s\n", r.clock_name.c_str(), r.ns_per_tick,
                r.clock_name == "rdtsc"
                    ? (r.tsc_invariant ? ", invariant TSC" : ", TSC NOT invariant")
                    : "");
    std::printf(
        "  timer cost  p50 %.1f ns, p99 %.1f ns per sample (included in the latencies "
        "below)\n",
        r.floor_p50_ns, r.floor_p99_ns);
    if (r.correctness.checked) {
        std::printf("  book check  %s, best bid/offer stream hash %016" PRIx64 "\n",
                    r.correctness.clean ? "all invariants zero" : "INVARIANTS VIOLATED",
                    r.correctness.hash);
    }
    if (!r.perf_available) {
        std::printf("  counters    unavailable (%s)\n",
                    r.perf_error.empty() ? "perf_event_open failed" : r.perf_error.c_str());
    }

    std::printf("\n  %-6s %12s %9s %9s %9s %9s %11s", "run", "msgs/s", "mean ns", "p50 ns",
                "p99 ns", "p99.9 ns", "max ns");
    if (r.perf_available) {
        std::printf(" %9s %9s %10s %10s", "cyc/msg", "ins/msg", "cmiss/msg", "bmiss/msg");
    }
    std::printf("\n");
    for (std::size_t i = 0; i < r.runs.size(); ++i) {
        const Run& x = r.runs[i];
        std::printf("  %-6zu %12.0f %9.1f %9.1f %9.1f %9.1f %11.0f", i + 1, x.msgs_per_s, x.mean_ns,
                    x.p50_ns, x.p99_ns, x.p999_ns, x.max_ns);
        if (r.perf_available) {
            std::printf(" %9.1f %9.1f %10.3f %10.3f", per_message(x, PerfCounters::Cycles),
                        per_message(x, PerfCounters::Instructions),
                        per_message(x, PerfCounters::CacheMisses),
                        per_message(x, PerfCounters::BranchMisses));
        }
        std::printf("\n");
    }
    std::printf("  %-6s %12.0f %9.1f %9.1f %9.1f %9.1f %11.0f\n", "median", rate.median,
                mean.median, p50.median, p99.median, p999.median, max.median);
    std::printf("  %-6s %12.0f %9.1f %9.1f %9.1f %9.1f %11.0f\n", "min", rate.min, mean.min,
                p50.min, p99.min, p999.min, max.min);
    std::printf("  %-6s %12.0f %9.1f %9.1f %9.1f %9.1f %11.0f\n", "max", rate.max, mean.max,
                p50.max, p99.max, p999.max, max.max);
    std::printf("\n  throughput spread across runs: %.1f%% of the median\n", rate.spread_percent());
    std::printf("  msgs/s and mean ns come from the pass with no per-message clock;\n");
    std::printf("  percentiles come from a second pass with one clock read per message.\n");

    for (const std::string& w : r.warnings) {
        std::printf("\n  WARNING: %s\n", w.c_str());
    }
}

bool write_json(const Report& r) {
    std::FILE* f = std::fopen(r.options.json_path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    const Summary rate = summarize(r.runs, [](const Run& x) { return x.msgs_per_s; });
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"label\": \"%s\",\n", json_escape(r.options.label).c_str());
    std::fprintf(f, "  \"file\": \"%s\",\n", json_escape(r.options.path).c_str());
    std::fprintf(f, "  \"file_bytes\": %zu,\n", r.file_bytes);
    std::fprintf(f, "  \"messages\": %" PRIu64 ",\n", r.messages);
    std::fprintf(f, "  \"workload\": \"%s\",\n", json_escape(r.workload_name).c_str());
    std::fprintf(f, "  \"in_memory_copy\": %s,\n", r.options.copy ? "true" : "false");
    std::fprintf(f, "  \"warmup_runs\": %d,\n", r.options.warmup);
    std::fprintf(f, "  \"cpu_model\": \"%s\",\n", json_escape(r.env.cpu_model).c_str());
    std::fprintf(f, "  \"logical_cpus\": %u,\n", r.env.logical_cpus);
    std::fprintf(f, "  \"memory\": \"%s\",\n", json_escape(r.env.memory).c_str());
    std::fprintf(f, "  \"kernel\": \"%s\",\n", json_escape(r.env.kernel).c_str());
    std::fprintf(f, "  \"hypervisor\": %s,\n", r.env.hypervisor ? "true" : "false");
    std::fprintf(f, "  \"wsl\": %s,\n", r.env.wsl ? "true" : "false");
    std::fprintf(f, "  \"pinned_cpu\": %d,\n", r.env.pinned ? r.env.cpu : -1);
    std::fprintf(f, "  \"governor\": \"%s\",\n", json_escape(r.env.governor).c_str());
    std::fprintf(f, "  \"compiler\": \"%s\",\n", json_escape(r.env.compiler).c_str());
    std::fprintf(f, "  \"build_type\": \"%s\",\n", json_escape(OBE_BUILD_TYPE).c_str());
    std::fprintf(f, "  \"build_flags\": \"%s\",\n", json_escape(OBE_BUILD_FLAGS).c_str());
    std::fprintf(f, "  \"clock\": \"%s\",\n", json_escape(r.clock_name).c_str());
    std::fprintf(f, "  \"ns_per_tick\": %.6f,\n", r.ns_per_tick);
    std::fprintf(f, "  \"tsc_invariant\": %s,\n", r.tsc_invariant ? "true" : "false");
    std::fprintf(f, "  \"timer_floor_p50_ns\": %.2f,\n", r.floor_p50_ns);
    std::fprintf(f, "  \"timer_floor_p99_ns\": %.2f,\n", r.floor_p99_ns);
    if (r.correctness.checked) {
        std::fprintf(f, "  \"book_invariants_clean\": %s,\n",
                     r.correctness.clean ? "true" : "false");
        std::fprintf(f, "  \"bbo_hash\": \"%016" PRIx64 "\",\n", r.correctness.hash);
    }
    std::fprintf(f, "  \"median_msgs_per_s\": %.0f,\n", rate.median);
    std::fprintf(f, "  \"min_msgs_per_s\": %.0f,\n", rate.min);
    std::fprintf(f, "  \"max_msgs_per_s\": %.0f,\n", rate.max);
    std::fprintf(f, "  \"runs\": [\n");
    for (std::size_t i = 0; i < r.runs.size(); ++i) {
        const Run& x = r.runs[i];
        std::fprintf(f,
                     "    {\"msgs_per_s\": %.0f, \"seconds\": %.6f, \"mean_ns\": %.2f, "
                     "\"p50_ns\": %.2f, \"p99_ns\": %.2f, \"p999_ns\": %.2f, \"max_ns\": %.0f, "
                     "\"digest\": \"%016" PRIx64 "\"",
                     x.msgs_per_s, x.seconds, x.mean_ns, x.p50_ns, x.p99_ns, x.p999_ns, x.max_ns,
                     x.digest);
        for (std::size_t c = 0; c < PerfCounters::kCount; ++c) {
            if (x.perf.valid[c]) {
                std::fprintf(f, ", \"%s\": %" PRIu64, PerfCounters::kNames[c], x.perf.value[c]);
            }
        }
        std::fprintf(f, "}%s\n", i + 1 < r.runs.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    return std::fclose(f) == 0;
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

// One untimed replay with the hashing listener: the benchmark report carries
// proof that the book being timed was also correct.
Correctness check_book(std::span<const std::byte> buf) {
    auto manager =
        std::make_unique<book::BookManager<book::OrderStore, book::PriceLevels, book::BboHasher>>();
    feed::ItchParser parser(*manager);
    parser.parse(buf);
    Correctness c;
    c.checked = true;
    c.counters = manager->counters();
    c.audit = manager->audit();
    c.clean = c.counters.clean() && c.audit.clean();
    c.hash = manager->listener().combined();
    return c;
}

template <class Workload, class Clock>
int run_workload(const Options& opt, std::span<const std::byte> buf, Report& report) {
    report.workload_name = Workload::kName;
    report.workload_description = Workload::kDescription;
    report.clock_name = Clock::kName;
    report.ns_per_tick = Clock::ns_per_tick();
    report.tsc_invariant = util::tsc_is_invariant();

    {
        const auto floor = timer_floor<Clock>(5'000'000);
        report.floor_p50_ns = static_cast<double>(floor->percentile(50)) * report.ns_per_tick;
        report.floor_p99_ns = static_cast<double>(floor->percentile(99)) * report.ns_per_tick;
    }

    PerfCounters perf;
    report.perf_available = perf.any_available();
    report.perf_error = perf.error();

    std::uint64_t reference_digest = 0;
    for (int i = 0; i < opt.warmup + opt.runs; ++i) {
        const ThroughputPass tp = throughput_pass<Workload>(buf, perf);
        const std::unique_ptr<LatencyPass> lp = latency_pass<Workload, Clock>(buf);
        if (!tp.ok || !lp->ok || tp.messages != report.messages) {
            std::fprintf(stderr, "error: a timed pass did not process the whole file\n");
            return 2;
        }
        if (i == 0) {
            reference_digest = tp.digest;
        } else if (tp.digest != reference_digest) {
            report.warnings.emplace_back(
                "runs produced different digests: the workload is not deterministic");
        }
        if (i < opt.warmup) {
            continue;
        }
        Run run;
        run.messages = tp.messages;
        run.seconds = tp.seconds;
        run.msgs_per_s = tp.seconds > 0 ? static_cast<double>(tp.messages) / tp.seconds : 0;
        run.mean_ns = tp.messages > 0 ? tp.seconds * 1e9 / static_cast<double>(tp.messages) : 0;
        run.p50_ns = static_cast<double>(lp->ticks.percentile(50)) * report.ns_per_tick;
        run.p99_ns = static_cast<double>(lp->ticks.percentile(99)) * report.ns_per_tick;
        run.p999_ns = static_cast<double>(lp->ticks.percentile(99.9)) * report.ns_per_tick;
        run.max_ns = static_cast<double>(lp->ticks.max()) * report.ns_per_tick;
        run.digest = tp.digest;
        run.perf = tp.perf;
        report.runs.push_back(run);
    }
    return 0;
}

int run(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = i + 1 < argc;
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--handler" && has_next) {
            opt.handler = argv[++i];
        } else if (arg == "--clock" && has_next) {
            opt.clock = argv[++i];
        } else if (arg == "--json" && has_next) {
            opt.json_path = argv[++i];
        } else if (arg == "--label" && has_next) {
            opt.label = argv[++i];
        } else if (arg == "--runs" && has_next && parse_int(argv[i + 1], opt.runs)) {
            ++i;
        } else if (arg == "--warmup" && has_next && parse_int(argv[i + 1], opt.warmup)) {
            ++i;
        } else if (arg == "--cpu" && has_next && parse_int(argv[i + 1], opt.cpu)) {
            ++i;
        } else if (arg == "--no-copy") {
            opt.copy = false;
        } else if (!arg.starts_with("-") && opt.path.empty()) {
            opt.path = arg;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (opt.path.empty() || opt.runs < 1 || opt.warmup < 0 ||
        (opt.handler != "book" && opt.handler != "parse") ||
        (opt.clock != "tsc" && opt.clock != "steady")) {
        return usage(stderr, 1);
    }

    Report report;
    report.options = opt;

    // Pin before loading, so the pages are first touched from the CPU (and on
    // a multi-socket machine, the memory node) the benchmark will run on.
    bool pinned = false;
    if (opt.cpu >= 0) {
        pinned = util::pin_to_cpu(opt.cpu);
        if (!pinned) {
            std::fprintf(stderr, "error: cannot pin to CPU %d\n", opt.cpu);
            return 1;
        }
    }
    report.env = gather_environment(opt, pinned);

    // Load. Nothing below this block touches the disk.
    std::unique_ptr<MemoryCopy> copy;
    std::unique_ptr<util::MappedFile> mapping;
    std::span<const std::byte> buf;
    if (opt.copy) {
        copy = std::make_unique<MemoryCopy>(opt.path);
        buf = copy->bytes();
    } else {
        mapping = std::make_unique<util::MappedFile>(opt.path, util::MappedFile::Advice::None);
        buf = mapping->bytes();
        static_cast<void>(touch_pages(buf));
    }
    if (util::looks_gzipped(buf)) {
        std::fprintf(stderr, "error: '%s' is gzip-compressed. Decompress it first.\n",
                     opt.path.c_str());
        return 1;
    }
    report.file_bytes = buf.size();

    // Validate the whole stream once, untimed, so a truncated file is reported
    // as such and the timed loops never take an error path.
    {
        feed::HandlerBase nothing;
        feed::ItchParser parser(nothing);
        const feed::ParseResult result = parser.parse(buf);
        if (!result.ok()) {
            const std::string_view why = feed::to_string(result.status);
            std::fprintf(stderr, "error: %.*s at byte offset %zu, after %" PRIu64 " messages\n",
                         static_cast<int>(why.size()), why.data(), result.offset, result.messages);
            return 2;
        }
        report.messages = result.messages;
    }
    if (report.messages == 0) {
        std::fprintf(stderr, "error: the file contains no messages\n");
        return 1;
    }

    if (!is_release_build() || is_instrumented_build()) {
        report.warnings.emplace_back(
            "this is not a Release build (assertions or sanitizers are on). The numbers above "
            "are meaningless. Use the 'release' preset.");
    }
    if (!pinned) {
        report.warnings.emplace_back("the thread is not pinned. Pass --cpu N.");
    }
    if (opt.runs < 5) {
        report.warnings.emplace_back("fewer than 5 measured runs. The spec's minimum is 5.");
    }
    if (report.env.wsl || report.env.hypervisor) {
        report.warnings.emplace_back(
            "running in a virtual machine. Expect noisier timings than native Linux, and say so "
            "wherever these numbers are quoted.");
    }

    int status = 0;
    if (opt.handler == "book") {
        report.correctness = check_book(buf);
        status = opt.clock == "tsc"
                     ? run_workload<BookWorkload, util::TscClock>(opt, buf, report)
                     : run_workload<BookWorkload, util::SteadyClock>(opt, buf, report);
    } else {
        status = opt.clock == "tsc"
                     ? run_workload<ParseWorkload, util::TscClock>(opt, buf, report)
                     : run_workload<ParseWorkload, util::SteadyClock>(opt, buf, report);
    }
    if (status != 0) {
        return status;
    }

    if (report.clock_name == "rdtsc" && !report.tsc_invariant) {
        report.warnings.emplace_back(
            "this CPU does not report an invariant TSC. Use --clock steady for the percentiles.");
    }
    if (!report.perf_available) {
        report.warnings.emplace_back(
            "hardware counters are unavailable here, so there are no cache-miss or branch-miss "
            "figures. On native Linux, check /proc/sys/kernel/perf_event_paranoid.");
    }

    print_text(report);
    if (!opt.json_path.empty() && !write_json(report)) {
        std::fprintf(stderr, "error: cannot write '%s'\n", opt.json_path.c_str());
        return 1;
    }
    if (report.correctness.checked && !report.correctness.clean) {
        std::fprintf(stderr, "error: the book violated its invariants; run book_replay\n");
        return 3;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const obe::util::Unimplemented& e) {
        std::fprintf(stderr,
                     "not built yet: %s\n"
                     "The feed layer can be measured on its own with --handler parse.\n",
                     e.what());
        return 4;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
