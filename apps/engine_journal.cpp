// engine_journal: a matching engine that survives being killed.
//
//   engine_journal run   --journal FILE [options]
//   engine_journal check --journal FILE [--snapshot FILE] [--engine NAME]
//   engine_journal dump  --journal FILE [--from N] [--limit N]
//
// `run` drives a matching engine with seeded order flow and writes every
// request to FILE before the engine acts on it. If FILE is already there, the
// run first recovers: it loads the snapshot if one is given and sound, replays
// the journal from there, cuts off a torn last record, and then carries on
// from the request after the last one the journal holds, until the journal
// holds --commands requests in all. Kill it at any moment and start it again
// with the same options; when it finally ends, FILE is byte for byte the file
// a run that was never killed would have written. scripts/journal_crash_test.sh
// does exactly that.
//
// `check` replays a journal into a fresh engine and reports how the journal
// ends and what the engine then holds, without changing anything on disk.
// `dump` prints the records.
//
// The state digest printed by run and check identifies what the engine holds:
// its instruments, every resting order in the order it would trade, and its
// totals. Two engines that print the same digest are in the same state.
//
// The load generator in this program is a stand-in for the outside world,
// which does not crash when the engine does. To put it back where it was, a
// restarted run feeds it its own requests again from the first, against a
// second engine that is thrown away. That costs as much as replaying the whole
// journal, and it is a cost of the demonstration, not of recovery: the two are
// timed and printed separately.
//
// Exit status
//   run    0 the journal holds all the requests, 1 usage or I/O error,
//          3 the journal cannot be carried on, 4 the engine is not written yet
//   check  0 clean end, 2 torn tail (a run would cut it and carry on),
//          3 the journal cannot be used, 1 and 4 as above
//   dump   0, or 1, 2 and 3 as for check

#include <signal.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "obe/engine/concepts.hpp"
#include "obe/engine/engines.hpp"
#include "obe/engine/types.hpp"
#include "obe/feed/codec.hpp"
#include "obe/gen/order_flow.hpp"
#include "obe/journal/file.hpp"
#include "obe/journal/reader.hpp"
#include "obe/journal/records.hpp"
#include "obe/journal/recover.hpp"
#include "obe/journal/replay.hpp"
#include "obe/journal/snapshot.hpp"
#include "obe/journal/writer.hpp"
#include "obe/util/format.hpp"
#include "obe/util/mapped_file.hpp"
#include "obe/util/todo.hpp"

namespace {

using namespace obe;

struct Options {
    std::string mode;
    std::string journal;
    std::string snapshot;
    std::string engine = "reference";
    gen::FlowConfig flow;
    std::uint64_t commands = 200'000;
    std::uint32_t batch = 1;
    journal::SyncPolicy sync = journal::SyncPolicy::Never;
    std::uint64_t snapshot_every = 0;
    // A testing hook: see Destination.
    std::uint64_t kill_at_flush = 0;
    std::uint32_t kill_keep = 0;
    // dump
    std::uint64_t from = 1;
    std::uint64_t limit = ~std::uint64_t{0};
};

int usage(std::FILE* to, int status) {
    std::fprintf(
        to,
        "usage: engine_journal run   --journal FILE [options]\n"
        "       engine_journal check --journal FILE [--snapshot FILE] [--engine NAME]\n"
        "       engine_journal dump  --journal FILE [--from N] [--limit N]\n"
        "       engine_journal --list\n"
        "run: trade, writing each request to the journal first. Recovers from FILE if it is\n"
        "there, then carries on until it holds --commands requests.\n"
        "  --commands N         requests the journal should hold at the end (default 200000)\n"
        "  --seed N --symbols N --owners N --live N --bad N    the flow, as in flow_gen\n"
        "  --engine NAME        which matching engine (default reference)\n"
        "  --batch N            requests gathered before each write (default 1)\n"
        "  --sync never|flush   whether to fdatasync after each write (default never)\n"
        "  --snapshot FILE      load this snapshot when recovering, if it is sound\n"
        "  --snapshot-every N   and write it again every N requests\n"
        "  --kill-at-flush K --kill-keep PCT\n"
        "                       for the crash test: at the K-th write of this run, write only\n"
        "                       PCT percent of it and kill this process with SIGKILL\n"
        "check: replay FILE into a fresh engine. Changes nothing on disk.\n"
        "dump:  print the records, from record --from, at most --limit of them.\n");
    return status;
}

void list_engines() {
    engine::for_each_engine([]<class Impl>(std::type_identity<Impl>) {
        std::printf("%-10.*s %.*s\n", static_cast<int>(Impl::kName.size()), Impl::kName.data(),
                    static_cast<int>(Impl::kDescription.size()), Impl::kDescription.data());
    });
}

bool parse_u64(std::string_view text, std::uint64_t& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

void line(const char* label, std::uint64_t value) {
    std::printf("  %-26s %s\n", label, util::with_commas(value).c_str());
}
void line(const char* label, const std::string& value) {
    std::printf("  %-26s %s\n", label, value.c_str());
}

double millis_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

std::string millis(double ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f ms", ms);
    return buf;
}

// What recover() found, in words. Shared by run and check.
void describe(const journal::Recovered& found, bool repaired) {
    if (!found.snapshot_found) {
        line("snapshot", "none");
    } else if (found.snapshot_used) {
        line("snapshot", "loaded: " + util::with_commas(found.snapshot_orders) +
                             " resting orders, taken before record " +
                             util::with_commas(found.snapshot_sequence));
    } else {
        line("snapshot", "found but not usable: ignored");
    }
    if (!found.journal_found) {
        line("journal", "not there yet");
    } else {
        std::string how = util::with_commas(found.journal_bytes) + " bytes, ";
        how += journal::to_string(found.replay.status);
        if (found.replay.status == journal::ReadStatus::TornTail) {
            const std::size_t torn = found.journal_bytes - found.replay.good_bytes;
            how += " (" + util::with_commas(torn) + (repaired ? " bytes cut off)" : " bytes)");
        }
        line("journal", how);
    }
    line("records skipped", found.replay.skipped);
    line("records replayed", found.replay.applied);
    line("next record", found.replay.next_sequence);
}

// Passes reports to the generator only once it is told to. During recovery
// the engine repeats everything it said the first time, and the generator has
// already heard all of that from the engine that is thrown away.
struct Gate {
    gen::OrderFlow* flow = nullptr;
    bool open = false;

    void on_accepted(const engine::Accepted& r) {
        if (open) {
            flow->on_accepted(r);
        }
    }
    void on_executed(const engine::Executed& r) {
        if (open) {
            flow->on_executed(r);
        }
    }
    void on_cancelled(const engine::Cancelled& r) {
        if (open) {
            flow->on_cancelled(r);
        }
    }
    void on_replaced(const engine::Replaced& r) {
        if (open) {
            flow->on_replaced(r);
        }
    }
    void on_rejected(const engine::Rejected& r) {
        if (open) {
            flow->on_rejected(r);
        }
    }
};

// Sends the first `skip` requests to one engine and the rest to another. It is
// how a restarted run puts the generator back where it was: the requests the
// journal already holds go to the engine that is thrown away.
template <class Scratch, class Live>
struct Switch {
    Scratch* scratch;
    Live* live;
    std::uint64_t skip;

    [[nodiscard]] bool caught_up() const noexcept { return skip == 0; }

    bool add_instrument(Locate locate, const feed::Symbol& symbol, Nanos now) {
        if (skip > 0) {
            --skip;
            return scratch->add_instrument(locate, symbol, now);
        }
        return live->add_instrument(locate, symbol, now);
    }
    OrderId submit(const engine::NewOrder& order, Nanos now) {
        if (skip > 0) {
            --skip;
            return scratch->submit(order, now);
        }
        return live->submit(order, now);
    }
    bool cancel(engine::OwnerId owner, OrderId id, Nanos now) {
        if (skip > 0) {
            --skip;
            return scratch->cancel(owner, id, now);
        }
        return live->cancel(owner, id, now);
    }
    OrderId replace(engine::OwnerId owner, OrderId id, Qty qty, Price price, Nanos now) {
        if (skip > 0) {
            --skip;
            return scratch->replace(owner, id, qty, price, now);
        }
        return live->replace(owner, id, qty, price, now);
    }
};

// Where the journal's bytes go: the file, synced if asked.
//
// It also holds the crash test's hook. A process killed by hand dies between
// two system calls, so the file always ends where some write ended. The
// interesting crash is the one in the middle of a write, which a power cut
// produces and kill -9 almost never does. So the hook makes it: at the chosen
// write, only part of the batch is written, and then the process kills
// itself with SIGKILL, which no handler can catch and no cleanup follows.
class Destination {
 public:
    Destination(journal::JournalFile& file, const Options& opt) noexcept
        : file_(&file), sync_(opt.sync), kill_at_(opt.kill_at_flush), kill_keep_(opt.kill_keep) {}

    void operator()(std::span<const std::byte> bytes) {
        ++flushes_;
        if (flushes_ == kill_at_) {
            const std::size_t part = bytes.size() * kill_keep_ / 100;
            file_->write(bytes.first(part));
            ::kill(::getpid(), SIGKILL);
            ::_exit(137);  // not reached: SIGKILL is delivered before kill() returns
        }
        file_->write(bytes);
        if (sync_ == journal::SyncPolicy::EveryFlush) {
            file_->sync();
        }
    }

 private:
    journal::JournalFile* file_;
    journal::SyncPolicy sync_;
    std::uint64_t kill_at_;
    std::uint32_t kill_keep_;
    std::uint64_t flushes_ = 0;
};

template <class Engine>
bool write_snapshot(const Engine& engine, std::uint64_t next_sequence, const std::string& path) {
    if constexpr (engine::Restorable<Engine>) {
        journal::write_file_atomically(path,
                                       journal::encode(journal::capture(engine, next_sequence)));
        return true;
    } else {
        (void)engine;
        (void)next_sequence;
        (void)path;
        return false;
    }
}

template <class Impl>
int run_mode(const Options& opt) {
    using Engine = typename Impl::template Engine<Gate, engine::NullMarketData>;
    using Scratch = typename Impl::template Engine<gen::OrderFlow, engine::NullMarketData>;
    using Writer = journal::JournalWriter<Destination>;

    if (!opt.snapshot.empty() && !engine::Restorable<Engine>) {
        std::fprintf(stderr, "error: the '%.*s' engine cannot be saved to a snapshot\n",
                     static_cast<int>(Impl::kName.size()), Impl::kName.data());
        return 1;
    }

    gen::OrderFlow flow(opt.flow);
    Gate gate{&flow, false};
    engine::NullMarketData market;
    // An engine holds a book for every possible locate; keep it off the stack.
    const auto engine = std::make_unique<Engine>(gate, market);

    std::printf("engine_journal run (%.*s: %.*s)\n", static_cast<int>(Impl::kName.size()),
                Impl::kName.data(), static_cast<int>(Impl::kDescription.size()),
                Impl::kDescription.data());

    // --- Recover -------------------------------------------------------------
    const auto recovery_start = std::chrono::steady_clock::now();
    const journal::Recovered found = journal::recover(opt.journal, opt.snapshot, *engine);
    const double recovery_ms = millis_since(recovery_start);
    std::printf("\nrecovery\n");
    describe(found, /*repaired=*/true);
    line("recovery took", millis(recovery_ms));
    // So that a run that is killed has still said what it recovered from.
    std::fflush(stdout);
    if (!found.ok()) {
        std::printf("\nRESULT: the journal cannot be carried on: %.*s\n",
                    static_cast<int>(journal::to_string(found.replay.status).size()),
                    journal::to_string(found.replay.status).data());
        return 3;
    }

    // The requests of a run, numbered as the journal numbers them: first one
    // to open each instrument, then the commands.
    const std::uint64_t total = flow.config().symbols + opt.commands;
    const std::uint64_t held = found.next_sequence() - 1;
    if (held > total) {
        std::printf("\nRESULT: the journal holds %s requests, more than the %s asked for\n",
                    util::with_commas(held).c_str(), util::with_commas(total).c_str());
        return 3;
    }

    // --- Carry on ------------------------------------------------------------
    journal::JournalFile file(opt.journal);
    Writer writer(Destination(file, opt), found.next_sequence(), found.file_is_empty());
    journal::Journaled journaled(*engine, writer, opt.batch);

    // From here on the engine's reports are news to the generator.
    gate.open = true;
    auto scratch = held > 0 ? std::make_unique<Scratch>(flow, market) : nullptr;
    Switch<Scratch, journal::Journaled<Engine, Writer>> target{scratch.get(), &journaled, held};

    std::uint64_t snapshots = 0;
    double catch_up_ms = 0;
    const auto catch_up_start = std::chrono::steady_clock::now();
    auto run_start = catch_up_start;

    // Called after every request: notices the moment the generator has caught
    // up, and takes the snapshots.
    const auto after = [&] {
        if (!target.caught_up()) {
            return;
        }
        if (scratch != nullptr) {
            // This request was the last one the journal already held.
            scratch.reset();
            catch_up_ms = millis_since(catch_up_start);
            run_start = std::chrono::steady_clock::now();
            return;
        }
        if (opt.snapshot_every != 0 && !opt.snapshot.empty() &&
            (writer.next_sequence() - 1) % opt.snapshot_every == 0) {
            // The journal first, and onto the disk: a snapshot must never be
            // ahead of the journal it goes with (see obe/journal/recover.hpp).
            journaled.commit();
            file.sync();
            if (write_snapshot(*engine, writer.next_sequence(), opt.snapshot)) {
                ++snapshots;
            }
        }
    };

    // open() makes one request per instrument, so `after` cannot run between
    // them; it runs once at the end, which is enough for both of its jobs.
    flow.open(target);
    after();
    for (std::uint64_t i = 0; i < opt.commands; ++i) {
        gen::apply(target, flow.next());
        after();
    }
    journaled.commit();
    if (opt.sync == journal::SyncPolicy::EveryFlush) {
        file.sync();
    }
    const double run_ms = millis_since(run_start);

    if (held > 0) {
        line("generator caught up in", millis(catch_up_ms));
    }
    std::printf("\nthis run\n");
    line("requests", writer.records());
    line("writes to the journal", file.writes());
    line("syncs", file.syncs());
    line("snapshots written", snapshots);
    line("took", millis(run_ms));

    const engine::EngineStats stats = engine->stats();
    std::printf("\njournal\n");
    line("records", writer.next_sequence() - 1);
    line("bytes", file.size());
    std::printf("\nengine\n");
    line("orders accepted", stats.accepted);
    line("requests rejected", stats.rejected);
    line("trades", stats.trades);
    line("orders resting", engine->open_orders());

    std::printf("\nstate digest: %016" PRIx64 "\n", journal::state_digest(*engine));
    if (flow.live() != engine->open_orders()) {
        // The generator follows the book through the reports alone. After a
        // recovery this also says that it was put back where it had been.
        std::printf(
            "\nRESULT: the generator and the engine disagree (%zu orders by the "
            "generator, %zu resting)\n",
            flow.live(), engine->open_orders());
        return 3;
    }
    std::printf("\nRESULT: ok\n");
    return 0;
}

template <class Impl>
int check_mode(const Options& opt) {
    using Engine = typename Impl::template Engine<engine::NullReports, engine::NullMarketData>;
    engine::NullReports reports;
    engine::NullMarketData market;
    const auto engine = std::make_unique<Engine>(reports, market);

    const auto start = std::chrono::steady_clock::now();
    const journal::Recovered found =
        journal::recover(opt.journal, opt.snapshot, *engine, /*repair=*/false);
    const double ms = millis_since(start);

    std::printf("engine_journal check (%.*s)\n", static_cast<int>(Impl::kName.size()),
                Impl::kName.data());
    describe(found, /*repaired=*/false);
    line("took", millis(ms));
    if (!found.journal_found) {
        std::printf("\nRESULT: no journal at '%s'\n", opt.journal.c_str());
        return 1;
    }
    const engine::EngineStats stats = engine->stats();
    line("orders accepted", stats.accepted);
    line("trades", stats.trades);
    line("orders resting", engine->open_orders());
    std::printf("\nstate digest: %016" PRIx64 "\n", journal::state_digest(*engine));

    if (found.replay.clean()) {
        std::printf("\nRESULT: clean\n");
        return 0;
    }
    if (found.ok()) {
        std::printf("\nRESULT: torn tail. The digest is of the engine after record %s.\n",
                    util::with_commas(found.replay.next_sequence - 1).c_str());
        return 2;
    }
    std::printf("\nRESULT: not usable: %.*s at byte %s\n",
                static_cast<int>(journal::to_string(found.replay.status).size()),
                journal::to_string(found.replay.status).data(),
                util::with_commas(found.replay.good_bytes).c_str());
    return 3;
}

const char* kind_name(char kind) {
    switch (static_cast<engine::OrderKind>(static_cast<std::uint8_t>(kind))) {
        case engine::OrderKind::Limit:
            return "limit";
        case engine::OrderKind::Market:
            return "market";
    }
    return "kind?";
}

const char* tif_name(char tif) {
    switch (static_cast<engine::TimeInForce>(static_cast<std::uint8_t>(tif))) {
        case engine::TimeInForce::Day:
            return "day";
        case engine::TimeInForce::ImmediateOrCancel:
            return "ioc";
        case engine::TimeInForce::FillOrKill:
            return "fok";
    }
    return "tif?";
}

int dump_mode(const Options& opt) {
    const util::MappedFile file(opt.journal);
    journal::JournalReader reader(file.bytes());
    journal::RecordView record;
    journal::ReadStatus status = reader.next(record);
    std::uint64_t printed = 0;
    for (; status == journal::ReadStatus::Ok; status = reader.next(record)) {
        if (record.seq < opt.from || printed >= opt.limit) {
            continue;
        }
        ++printed;
        std::printf("%10" PRIu64 "  %c  ", record.seq, record.type);
        switch (record.type) {
            case journal::AddInstrument::kType: {
                const auto r = feed::decode<journal::AddInstrument>(record.body);
                std::printf("t=%" PRIu64 "  open locate %u as %.*s\n", r.now,
                            static_cast<unsigned>(r.locate),
                            static_cast<int>(r.symbol.view().size()), r.symbol.view().data());
                break;
            }
            case journal::Submit::kType: {
                const auto r = feed::decode<journal::Submit>(record.body);
                std::printf("t=%" PRIu64 "  owner %u token %" PRIu64
                            " locate %u  %c %u @ %s  %s %s\n",
                            r.now, r.owner, r.token, static_cast<unsigned>(r.locate),
                            static_cast<char>(r.side), r.qty, util::format_price(r.price).c_str(),
                            kind_name(r.kind), tif_name(r.tif));
                break;
            }
            case journal::Cancel::kType: {
                const auto r = feed::decode<journal::Cancel>(record.body);
                std::printf("t=%" PRIu64 "  owner %u cancels order %" PRIu64 "\n", r.now, r.owner,
                            r.order_id);
                break;
            }
            default: {
                const auto r = feed::decode<journal::Replace>(record.body);
                std::printf("t=%" PRIu64 "  owner %u replaces order %" PRIu64 " with %u @ %s\n",
                            r.now, r.owner, r.order_id, r.qty, util::format_price(r.price).c_str());
                break;
            }
        }
    }
    std::printf("%s records, first %s; the journal ends with: %.*s (%s of %s bytes are good)\n",
                util::with_commas(reader.next_sequence() - reader.first_sequence()).c_str(),
                util::with_commas(reader.first_sequence()).c_str(),
                static_cast<int>(journal::to_string(status).size()),
                journal::to_string(status).data(), util::with_commas(reader.offset()).c_str(),
                util::with_commas(file.size()).c_str());
    if (status == journal::ReadStatus::End) {
        return 0;
    }
    return status == journal::ReadStatus::TornTail ? 2 : 3;
}

int run(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = i + 1 < argc;
        std::uint64_t value = 0;
        const bool has_value = has_next && parse_u64(argv[i + 1], value);
        if (arg == "-h" || arg == "--help") {
            return usage(stdout, 0);
        }
        if (arg == "--list") {
            list_engines();
            return 0;
        }
        if (i == 1 && (arg == "run" || arg == "check" || arg == "dump")) {
            opt.mode = arg;
        } else if (arg == "--journal" && has_next) {
            opt.journal = argv[++i];
        } else if (arg == "--snapshot" && has_next) {
            opt.snapshot = argv[++i];
        } else if (arg == "--engine" && has_next) {
            opt.engine = argv[++i];
        } else if (arg == "--sync" && has_next) {
            const std::string_view how = argv[++i];
            if (how == "never") {
                opt.sync = journal::SyncPolicy::Never;
            } else if (how == "flush") {
                opt.sync = journal::SyncPolicy::EveryFlush;
            } else {
                std::fprintf(stderr, "error: --sync takes 'never' or 'flush'\n");
                return usage(stderr, 1);
            }
        } else if (arg == "--seed" && has_value) {
            opt.flow.seed = value;
            ++i;
        } else if (arg == "--commands" && has_value) {
            opt.commands = value;
            ++i;
        } else if (arg == "--symbols" && has_value) {
            opt.flow.symbols = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--owners" && has_value) {
            opt.flow.owners = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--live" && has_value) {
            opt.flow.target_live_orders = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--bad" && has_value) {
            opt.flow.bad_per_million = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--batch" && has_value && value >= 1 && value <= 1'000'000) {
            opt.batch = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--snapshot-every" && has_value) {
            opt.snapshot_every = value;
            ++i;
        } else if (arg == "--kill-at-flush" && has_value) {
            opt.kill_at_flush = value;
            ++i;
        } else if (arg == "--kill-keep" && has_value && value <= 100) {
            opt.kill_keep = static_cast<std::uint32_t>(value);
            ++i;
        } else if (arg == "--from" && has_value) {
            opt.from = value;
            ++i;
        } else if (arg == "--limit" && has_value) {
            opt.limit = value;
            ++i;
        } else {
            std::fprintf(stderr, "error: bad argument '%s'\n", argv[i]);
            return usage(stderr, 1);
        }
    }
    if (opt.mode.empty()) {
        std::fprintf(stderr, "error: say what to do: run, check or dump\n");
        return usage(stderr, 1);
    }
    if (opt.journal.empty()) {
        std::fprintf(stderr, "error: --journal FILE is required\n");
        return usage(stderr, 1);
    }
    if (opt.mode == "dump") {
        return dump_mode(opt);
    }

    int status = 1;
    const bool known = engine::with_engine(opt.engine, [&]<class Impl>(std::type_identity<Impl>) {
        status = opt.mode == "run" ? run_mode<Impl>(opt) : check_mode<Impl>(opt);
    });
    if (!known) {
        std::fprintf(stderr, "error: no engine called '%s'. Try --list.\n", opt.engine.c_str());
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
