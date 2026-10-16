#!/usr/bin/env bash
# Every check and measurement of the project on one real Nasdaq day, in one
# run: the success criteria of the spec (section 3.3), the experiments of the
# optimization log, and the benchmarks behind the README's tables.
#
#   scripts/real_data_evals.sh [options]
#
#   scripts/real_data_evals.sh                       everything (one to three hours)
#   scripts/real_data_evals.sh --quick               a short pass to try it out
#   scripts/real_data_evals.sh --only gateway,sim    some stages again
#   scripts/real_data_evals.sh --list                the stages
#
# Options:
#   --data FILE      the uncompressed ITCH file. Default: data/<day> in the
#                    repository or in the work directory; if it is not there,
#                    scripts/fetch_data.sh downloads and decompresses it.
#   --day NAME       the file name on Nasdaq's server, without .gz
#                    (default 01302019.NASDAQ_ITCH50)
#   --work DIR       builds, the copy of the data and scratch files
#                    (default ~/obe-evals). Keep it on the Linux file system:
#                    under WSL2, /mnt/c and /mnt/f are slow network mounts.
#   --results DIR    where the reports go (default results/real-<time> in the
#                    repository, which git ignores)
#   --cpus A,B,C     three CPUs to pin threads to (default 2,4,6). Choose
#                    three different physical cores, away from CPU 0.
#   --symbol S       the security mm_sim quotes (default AAPL)
#   --window A-B     the time of day mm_sim quotes in (default 09:30-16:00)
#   --runs N         measured runs per benchmark row (default 5, the spec's
#                    minimum; --quick uses 2)
#   --quick          fewer runs, shorter loads, no PGO and no billion-item
#                    stress run: to check that everything works, not to quote
#   --only LIST      run only these stages (comma separated)
#   --skip LIST      run every stage but these
#
# Environment: CXX picks the compiler (CXX=clang++-18, say); OBE_CMAKE_ARGS
# is added to every configure (for example a local copy of the dependencies).
#
# Every stage writes into its own directory under the results directory and
# records its exit status in status.tsv. A stage that fails does not stop the
# others, except that nothing runs without a build and a data file. The last
# stage, summary, writes SUMMARY.md: the criteria with pass or fail, and the
# numbers.
#
# What it needs: Linux or WSL2, CMake 3.21+, Ninja, GCC 13+ or Clang 18+,
# Python 3, curl, gunzip, about 40 GB free in the work directory, and network
# access the first time (CMake fetches GoogleTest and Google Benchmark, and the
# data file is about 4 GB). Optional: taskset (CPU pinning of the exchange),
# zstd (the general-purpose comparison for the tick store), matplotlib (the
# latency plot), perf (hardware counters; usually absent under WSL2).
#
# On the numbers: they are for this machine, labelled with it (stage env), and
# measured with docs/benchmark-method.md. Under WSL2 the CPUs are virtual: the
# pinning is to virtual CPUs, which Windows may move between physical cores,
# and the disk is a virtual disk. Say so wherever a number is quoted.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAY="01302019.NASDAQ_ITCH50"
DATA=""
WORK="${HOME}/obe-evals"
RESULTS=""
CPUS="2,4,6"
SYMBOL="AAPL"
WINDOW="09:30-16:00"
RUNS=""
QUICK=0
ONLY=""
SKIP=""

STAGES=(env build data counts replay profile diff bench lto pgo engine queues tsan pipeline
        gateway transports sim journal ticks summary)

describe() {
    case "$1" in
        env) echo "the machine, the system, the compilers: what every number belongs to" ;;
        build) echo "configure and build the release tree in the work directory" ;;
        data) echo "find, download or decompress the day, and copy it to the work directory" ;;
        counts) echo "criterion 1: itch_stats against the independent Python counter" ;;
        replay) echo "criterion 2: a full-day replay with every invariant counter at zero" ;;
        profile) echo "feed_profile: the facts the experiments rest on" ;;
        diff) echo "criterion 3: every book implementation publishes the reference's stream" ;;
        bench) echo "criteria 4 and 5: replay_bench on every implementation and option" ;;
        lto) echo "experiment 10a: the fastest book with link-time optimization" ;;
        pgo) echo "experiment 10b: the fastest book with profile-guided optimization" ;;
        engine) echo "criterion 7 and the engines: round trip, equality, engine_bench" ;;
        queues) echo "criterion 6, half one: queue_bench and the billion-item ordered run" ;;
        tsan) echo "criterion 6, half two: the queues and the pipeline under ThreadSanitizer" ;;
        pipeline) echo "the three-thread pipeline against one thread, on the day" ;;
        gateway) echo "criterion 8: the latency curve of exchange, subscriber and load generator" ;;
        transports) echo "epoll against io_uring: system calls per request" ;;
        sim) echo "the market-making simulation on one security, three strategies" ;;
        journal) echo "the crash test and what journaling costs on this disk" ;;
        ticks) echo "the tick store on the day: sizes, checks, speeds, against zstd" ;;
        summary) echo "SUMMARY.md: the criteria, pass or fail, and the numbers" ;;
    esac
}

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --data) DATA="$2"; shift 2 ;;
        --day) DAY="$2"; shift 2 ;;
        --work) WORK="$2"; shift 2 ;;
        --results) RESULTS="$2"; shift 2 ;;
        --cpus) CPUS="$2"; shift 2 ;;
        --symbol) SYMBOL="$2"; shift 2 ;;
        --window) WINDOW="$2"; shift 2 ;;
        --runs) RUNS="$2"; shift 2 ;;
        --quick) QUICK=1; shift ;;
        --only) ONLY="$2"; shift 2 ;;
        --skip) SKIP="$2"; shift 2 ;;
        --list)
            for s in "${STAGES[@]}"; do printf '  %-11s %s\n' "$s" "$(describe "$s")"; done
            exit 0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option '$1'" >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "$RUNS" ]]; then
    RUNS=$([[ "$QUICK" -eq 1 ]] && echo 2 || echo 5)
fi
IFS=, read -r CPU_A CPU_B CPU_C <<<"$CPUS"
if [[ -z "${CPU_A:-}" || -z "${CPU_B:-}" || -z "${CPU_C:-}" ]]; then
    echo "error: --cpus needs three CPU numbers, such as 2,4,6" >&2
    exit 1
fi
SIM_FROM="${WINDOW%-*}"
SIM_TO="${WINDOW#*-}"

mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
if [[ -z "$RESULTS" ]]; then
    RESULTS="$REPO/results/real-$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p "$RESULTS"
RESULTS="$(cd "$RESULTS" && pwd)"
B="$WORK/build/release"
STATUS="$RESULTS/status.tsv"
[[ -f "$STATUS" ]] || printf 'stage\tstatus\tseconds\tnote\n' >"$STATUS"
SETTINGS="$RESULTS/settings.env"

selected() {
    local s="$1"
    if [[ -n "$ONLY" ]]; then
        [[ ",$ONLY," == *",$s,"* ]] || return 1
    fi
    [[ ",$SKIP," == *",$s,"* ]] && return 1
    return 0
}

# Writes a line of the status table. Later lines for a stage win.
record() {
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "${4:-}" >>"$STATUS"
}

say() { printf '\n==> %s\n' "$*"; }

# Runs a command, prints it first, and returns its status.
run() {
    printf '+ %s\n' "$*"
    "$@"
}

# The data file and the order-table size feed_profile found, once known.
F=""
RESERVE=0

# replay_bench and pipeline_bench time against a private in-memory copy of
# the file, which needs as much free memory as the file is large. Where there
# is not that much, they read the page cache instead, and the report says so.
copy_option() {
    local size avail
    size=$(stat -c %s "$F" 2>/dev/null || echo 0)
    avail=$(awk '/^MemAvailable:/ {print $2 * 1024}' /proc/meminfo 2>/dev/null || echo 0)
    if [[ "$avail" -gt 0 && $((size + size / 4)) -gt "$avail" ]]; then
        echo "--no-copy"
    fi
}
load_settings() {
    if [[ -f "$SETTINGS" ]]; then
        # shellcheck disable=SC1090
        source "$SETTINGS"
    fi
}
save_setting() {
    grep -v "^$1=" "$SETTINGS" 2>/dev/null >"$SETTINGS.tmp" || true
    printf '%s=%q\n' "$1" "$2" >>"$SETTINGS.tmp"
    mv "$SETTINGS.tmp" "$SETTINGS"
}

need_build() {
    if [[ ! -x "$B/apps/book_replay" ]]; then
        echo "error: no build in $B. Run the build stage first." >&2
        return 1
    fi
}
need_data() {
    if [[ -z "$F" || ! -f "$F" ]]; then
        echo "error: no data file. Run the data stage first, or pass --data." >&2
        return 1
    fi
}

# --- stages ---------------------------------------------------------------

stage_env() {
    {
        echo "date (UTC)       $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "commit           $(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)"
        if ! git -C "$REPO" diff --quiet 2>/dev/null; then
            echo "working tree     has uncommitted changes"
        fi
        echo "kernel           $(uname -srm)"
        if grep -qi microsoft /proc/version 2>/dev/null; then
            echo "environment      WSL2 (virtual CPUs and a virtual disk)"
        else
            echo "environment      native Linux or another VM"
        fi
        echo "cpu              $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
        echo "logical cpus     $(nproc)"
        echo "memory           $(free -h | awk '/^Mem:/ {print $2}')"
        echo "thp              $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo unknown)"
        echo "governor         $(cat /sys/devices/system/cpu/cpu"$CPU_A"/cpufreq/scaling_governor 2>/dev/null || echo 'not exposed')"
        echo "pinned cpus      $CPUS"
        echo "c++ compiler     $("${CXX:-c++}" --version 2>/dev/null | head -n1)"
        echo "cmake            $(cmake --version 2>/dev/null | head -n1)"
        echo "ninja            $(ninja --version 2>/dev/null || echo missing)"
        echo "python           $(python3 --version 2>&1)"
        echo "perf             $(command -v perf >/dev/null && echo present || echo absent)"
        echo "taskset          $(command -v taskset >/dev/null && echo present || echo absent)"
        echo "zstd             $(command -v zstd >/dev/null && zstd --version | head -n1 || echo absent)"
    } | tee "$1/env.txt"
    if command -v lscpu >/dev/null; then
        lscpu -e >"$1/lscpu.txt" 2>&1 || true
        echo
        echo "CPU map (lscpu -e) in lscpu.txt: CPUs that share a CORE are hyper-threads of one core."
    fi
    local missing=0 version
    version="$("${CXX:-c++}" -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1)"
    if "${CXX:-c++}" --version 2>/dev/null | grep -qi clang; then
        if [[ "${version:-0}" -lt 18 ]]; then
            echo "warning: the project is developed with Clang 18 and GCC 13; this is Clang $version" >&2
        fi
    elif [[ "${version:-0}" -lt 13 ]]; then
        echo "warning: the project is developed with GCC 13 and Clang 18; this is GCC $version." >&2
        echo "  On Ubuntu 24.04: sudo apt install g++-13, then run with CXX=g++-13." >&2
    fi
    for tool in cmake ninja python3 curl gunzip; do
        if ! command -v "$tool" >/dev/null; then
            echo "missing: $tool" >&2
            missing=1
        fi
    done
    if [[ "$missing" -ne 0 ]]; then
        echo "Install what is missing, for example on Ubuntu:" >&2
        echo "  sudo apt install cmake ninja-build python3 curl gzip g++-13" >&2
        return 1
    fi
}

stage_build() {
    cd "$REPO" || return 1
    # OBE_CMAKE_ARGS is meant to be split into words.
    # shellcheck disable=SC2086
    run cmake --preset release -B "$B" -DOBE_BUILD_TESTS=OFF -DOBE_WARNINGS_AS_ERRORS=OFF ${OBE_CMAKE_ARGS:-} >"$1/configure.txt" 2>&1 ||
        { cat "$1/configure.txt"; return 1; }
    run cmake --build "$B" 2>&1 | tail -n 5
    [[ "${PIPESTATUS[0]}" -eq 0 ]] || return 1
    "$B/apps/exchange_server" --list-io | tee "$1/transports.txt"
}

stage_data() {
    local candidate="" checked="not here (an uncompressed file was given or found)"
    if [[ -n "$DATA" ]]; then
        candidate="$DATA"
    elif [[ -f "$WORK/data/$DAY" ]]; then
        candidate="$WORK/data/$DAY"
    elif [[ -f "$REPO/data/$DAY" ]]; then
        candidate="$REPO/data/$DAY"
    elif [[ -f "$REPO/data/$DAY.gz" ]]; then
        mkdir -p "$WORK/data"
        say "decompressing $REPO/data/$DAY.gz"
        gunzip -c "$REPO/data/$DAY.gz" >"$WORK/data/$DAY.part" && mv "$WORK/data/$DAY.part" "$WORK/data/$DAY" ||
            return 1
        candidate="$WORK/data/$DAY"
        checked="gzip CRC while decompressing"
    else
        say "downloading $DAY.gz from Nasdaq (about 5 GB)"
        run "$REPO/scripts/fetch_data.sh" -f "$DAY.gz" -d "$WORK/data" -x || return 1
        candidate="$WORK/data/$DAY"
        checked="by fetch_data.sh: md5 where Nasdaq serves one, else size and gzip CRC"
    fi
    if [[ ! -f "$candidate" ]]; then
        echo "error: $candidate is not a file" >&2
        return 1
    fi
    # Reading the day through a Windows mount is several times slower, and
    # replay_bench's first pass would be measuring the mount.
    case "$(cd "$(dirname "$candidate")" && pwd)" in
        /mnt/*)
            mkdir -p "$WORK/data"
            say "copying $candidate to $WORK/data (the Linux file system)"
            cp "$candidate" "$WORK/data/$(basename "$candidate")" || return 1
            candidate="$WORK/data/$(basename "$candidate")"
            ;;
    esac
    F="$(cd "$(dirname "$candidate")" && pwd)/$(basename "$candidate")"
    save_setting F "$F"
    {
        echo "file             $F"
        echo "size (bytes)     $(stat -c %s "$F")"
        echo "integrity        $checked"
    } | tee "$1/data.txt"
}

stage_counts() {
    need_build && need_data || return 1
    run "$REPO/scripts/check_counts.sh" --stats "$B/apps/itch_stats" --file "$F" --workdir "$1"
}

stage_replay() {
    need_build && need_data || return 1
    run "$B/apps/itch_stats" "$F" | tee "$1/itch_stats.txt"
    run "$B/apps/book_replay" --hashes "$1/hashes_reference.txt" "$F" | tee "$1/book_replay.txt"
    return "${PIPESTATUS[0]}"
}

stage_profile() {
    need_build && need_data || return 1
    run "$B/apps/feed_profile" "$F" | tee "$1/feed_profile.txt"
    local status="${PIPESTATUS[0]}"
    local peak
    peak="$(awk '/most resting at once/ {gsub(",", "", $NF); print $NF}' "$1/feed_profile.txt")"
    if [[ -n "$peak" ]]; then
        # A little room over the peak, so a pre-sized table never grows.
        RESERVE=$((peak + peak / 10))
        save_setting RESERVE "$RESERVE"
        echo "order table pre-size for the benchmarks: $RESERVE"
    fi
    return "$status"
}

stage_diff() {
    need_build && need_data || return 1
    local reserve=()
    [[ "$RESERVE" -gt 0 ]] && reserve=(--reserve "$RESERVE")
    run "$REPO/scripts/diff_books.sh" --replay "$B/apps/book_replay" --file "$F" --workdir "$1" "${reserve[@]}"
}

# One replay_bench row: label, then options.
bench_row() {
    local dir="$1" label="$2"
    shift 2
    # shellcheck disable=SC2046
    run "$B/bench/replay_bench" "$F" --cpu "$CPU_A" --runs "$RUNS" --label "$label" \
        --json "$dir/$label.json" $(copy_option) "$@" >"$dir/$label.txt" 2>&1
    local status=$?
    tail -n 25 "$dir/$label.txt"
    return "$status"
}

stage_bench() {
    need_build && need_data || return 1
    local failed=0 reserve=()
    [[ "$RESERVE" -gt 0 ]] && reserve=(--reserve "$RESERVE")
    if [[ -n "$(copy_option)" ]]; then
        echo "NOTE: the file does not fit in free memory with room to spare; every row reads" \
            "the page cache (--no-copy). Give WSL2 more memory (.wslconfig) to change that." |
            tee "$1/NOTE.txt"
    fi
    bench_row "$1" parse --handler parse || failed=1
    bench_row "$1" reference --impl reference || failed=1
    bench_row "$1" pooled --impl pooled || failed=1
    bench_row "$1" flat-store --impl flat-store || failed=1
    bench_row "$1" vector-levels --impl vector-levels || failed=1
    bench_row "$1" flat-vector --impl flat-vector || failed=1
    if [[ "${#reserve[@]}" -gt 0 ]]; then
        bench_row "$1" flat-vector-reserve --impl flat-vector "${reserve[@]}" || failed=1
        bench_row "$1" flat-vector-reserve-prefetch --impl flat-vector "${reserve[@]}" --prefetch || failed=1
        bench_row "$1" reference-reserve --impl reference "${reserve[@]}" || failed=1
    fi
    for pair in "reference flat-store" "reference vector-levels" "reference pooled" \
                "reference flat-vector" "flat-vector flat-vector-reserve" \
                "flat-vector-reserve flat-vector-reserve-prefetch"; do
        read -r before after <<<"$pair"
        if [[ -f "$1/$before.json" && -f "$1/$after.json" ]]; then
            python3 "$REPO/scripts/compare_runs.py" "$1/$before.json" "$1/$after.json" \
                >"$1/compare_${before}_vs_${after}.md" 2>&1 || true
        fi
    done
    return "$failed"
}

stage_lto() {
    need_data || return 1
    cd "$REPO" || return 1
    local build="$WORK/build/release-lto"
    # OBE_CMAKE_ARGS is meant to be split into words.
    # shellcheck disable=SC2086
    run cmake --preset release-lto -B "$build" -DOBE_BUILD_TESTS=OFF -DOBE_WARNINGS_AS_ERRORS=OFF ${OBE_CMAKE_ARGS:-} >"$1/configure.txt" 2>&1 ||
        { cat "$1/configure.txt"; return 1; }
    run cmake --build "$build" --target replay_bench 2>&1 | tail -n 3
    local reserve=()
    [[ "$RESERVE" -gt 0 ]] && reserve=(--reserve "$RESERVE")
    # shellcheck disable=SC2046
    run "$build/bench/replay_bench" "$F" --cpu "$CPU_A" --runs "$RUNS" --label lto --impl flat-vector \
        "${reserve[@]}" $(copy_option) --json "$1/lto.json" >"$1/lto.txt" 2>&1
    local status=$?
    tail -n 25 "$1/lto.txt"
    local base="$RESULTS/bench/flat-vector${reserve:+-reserve}.json"
    [[ -f "$base" ]] && python3 "$REPO/scripts/compare_runs.py" "$base" "$1/lto.json" >"$1/compare.md" 2>&1
    return "$status"
}

stage_pgo() {
    need_data || return 1
    if [[ "$QUICK" -eq 1 ]]; then
        echo "skipped in --quick"
        return 0
    fi
    cd "$REPO" || return 1
    local build="$WORK/build/release-pgo"
    local reserve=()
    [[ "$RESERVE" -gt 0 ]] && reserve=(--reserve "$RESERVE")
    # The training run is a replay_bench run too: it needs the same
    # --no-copy when the file does not fit in memory.
    # shellcheck disable=SC2046
    BUILD="$build" run "$REPO/scripts/pgo_build.sh" "$F" --impl flat-vector "${reserve[@]}" \
        $(copy_option) >"$1/build.txt" 2>&1 || { tail -n 20 "$1/build.txt"; return 1; }
    # shellcheck disable=SC2046
    run "$build/bench/replay_bench" "$F" --cpu "$CPU_A" --runs "$RUNS" --label pgo --impl flat-vector \
        "${reserve[@]}" $(copy_option) --json "$1/pgo.json" >"$1/pgo.txt" 2>&1
    local status=$?
    tail -n 25 "$1/pgo.txt"
    echo "Trained on the same file it was measured on: the best case for PGO." | tee "$1/NOTE.txt"
    local base="$RESULTS/bench/flat-vector${reserve:+-reserve}.json"
    [[ -f "$base" ]] && python3 "$REPO/scripts/compare_runs.py" "$base" "$1/pgo.json" >"$1/compare.md" 2>&1
    return "$status"
}

stage_engine() {
    need_build || return 1
    local failed=0 commands=5000000
    [[ "$QUICK" -eq 1 ]] && commands=500000
    run "$REPO/scripts/flow_round_trip.sh" "$B/apps/flow_gen" "$B/apps/book_replay" "$1" \
        --commands "$commands" --symbols 500 --live 50000 >"$1/round_trip.txt" 2>&1 || failed=1
    tail -n 15 "$1/round_trip.txt"
    run "$REPO/scripts/diff_engines.sh" --flow-gen "$B/apps/flow_gen" -- --commands "$commands" \
        --symbols 500 >"$1/diff_engines.txt" 2>&1 || failed=1
    tail -n 8 "$1/diff_engines.txt"
    for engine in reference pooled; do
        run "$B/bench/engine_bench" --engine "$engine" --cpu "$CPU_A" --runs "$RUNS" \
            --json "$1/engine_$engine.json" >"$1/engine_$engine.txt" 2>&1 || failed=1
        tail -n 20 "$1/engine_$engine.txt"
        # The same with the stretch 2 instructions in the flow.
        run "$B/bench/engine_bench" --engine "$engine" --cpu "$CPU_A" --runs "$RUNS" \
            --icebergs 50000 --post-only 50000 --self-match 50000 \
            --json "$1/engine_${engine}_instructions.json" >"$1/engine_${engine}_instructions.txt" 2>&1 ||
            failed=1
    done
    return "$failed"
}

stage_queues() {
    need_build || return 1
    local failed=0 items=1000000000
    [[ "$QUICK" -eq 1 ]] && items=50000000
    for queue in mutex mutex-spin ring; do
        run "$B/bench/queue_bench" --queue "$queue" --cpus "$CPU_A,$CPU_B" --runs "$RUNS" \
            --json "$1/queue_$queue.json" >"$1/queue_$queue.txt" 2>&1 || failed=1
        tail -n 12 "$1/queue_$queue.txt"
    done
    # The same two threads as hyper-threads of one core, if the next CPU is
    # the sibling of the first (lscpu.txt says).
    run "$B/bench/queue_bench" --queue ring --cpus "$CPU_A,$((CPU_A + 1))" --runs "$RUNS" \
        --json "$1/queue_ring_siblings.json" >"$1/queue_ring_siblings.txt" 2>&1 || true
    run "$REPO/scripts/queue_stress.sh" "$B/bench/queue_bench" ring "$items" | tee "$1/stress_ring.txt"
    [[ "${PIPESTATUS[0]}" -eq 0 ]] || failed=1
    return "$failed"
}

stage_tsan() {
    cd "$REPO" || return 1
    local build="$WORK/build/tsan"
    # OBE_CMAKE_ARGS is meant to be split into words.
    # shellcheck disable=SC2086
    run cmake --preset tsan -B "$build" -DOBE_WARNINGS_AS_ERRORS=OFF ${OBE_CMAKE_ARGS:-} >"$1/configure.txt" 2>&1 || { cat "$1/configure.txt"; return 1; }
    run cmake --build "$build" --target obe_concurrency_tests obe_core_concurrency_tests 2>&1 |
        tail -n 3
    local failed=0
    for t in obe_concurrency_tests obe_core_concurrency_tests; do
        TSAN_OPTIONS=halt_on_error=1 run "$build/tests/$t" >"$1/$t.txt" 2>&1 || failed=1
        grep -E "PASSED|FAILED|ThreadSanitizer" "$1/$t.txt" | head -n 5
    done
    return "$failed"
}

stage_pipeline() {
    need_build && need_data || return 1
    local failed=0 reserve=()
    [[ "$RESERVE" -gt 0 ]] && reserve=(--reserve "$RESERVE")
    for queue in mutex ring; do
        # shellcheck disable=SC2046
        run "$B/bench/pipeline_bench" "$F" --queue "$queue" --impl flat-vector "${reserve[@]}" $(copy_option) \
            --cpus "$CPU_A,$CPU_B,$CPU_C" --runs "$RUNS" --json "$1/pipeline_$queue.json" \
            >"$1/pipeline_$queue.txt" 2>&1 || failed=1
        tail -n 15 "$1/pipeline_$queue.txt"
    done
    return "$failed"
}

# Waits up to $2 seconds for process $1 to end, then kills it. Returns its
# exit status (137 if it had to be killed).
wait_or_kill() {
    for _ in $(seq 1 "$(($2 * 10))"); do
        kill -0 "$1" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$1" 2>/dev/null; then
        echo "process $1 did not stop in $2 s: killing it" >&2
        kill -KILL "$1" 2>/dev/null
    fi
    wait "$1"
}

# Waits for a line matching $2 in file $1 for up to 20 s, while process $3 lives.
wait_for() {
    for _ in $(seq 1 200); do
        grep -q "$2" "$1" 2>/dev/null && return 0
        kill -0 "$3" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

# Sets PIN to the prefix that runs a command on CPU $1. A prefix and not a
# wrapper function: a function started with & runs in a subshell, $! is the
# subshell's, and a signal sent to it never reaches the program.
PIN=()
pin_to() {
    if command -v taskset >/dev/null; then
        PIN=(taskset -c "$1")
    else
        PIN=()
    fi
}

# One latency curve: label, then exchange_server options.
gateway_curve() {
    local dir="$1" label="$2"
    shift 2
    local rates="1000,5000,20000,50000,100000" seconds=10
    [[ "$QUICK" -eq 1 ]] && { rates="1000,10000"; seconds=2; }
    local listen_log="$dir/${label}_md_listen.txt" server_log="$dir/${label}_exchange.txt"
    # An upper bound for the subscriber, so that nothing can hang the run: every
    # rate with its warm-up, and a minute to spare.
    local n_rates bound
    n_rates=$(tr ',' '\n' <<<"$rates" | wc -l)
    bound=$((n_rates * (seconds + 1) + 60))
    pin_to "$CPU_C"
    "${PIN[@]}" "$B/apps/md_listen" --md 127.0.0.1:0 --session EVALS --seconds "$bound" \
        >"$listen_log" 2>&1 &
    local listen_pid=$!
    if ! wait_for "$listen_log" "^listening on" "$listen_pid"; then
        echo "md_listen did not start" >&2
        kill "$listen_pid" 2>/dev/null
        return 1
    fi
    local md_port
    md_port="$(sed -n 's/^listening on [0-9.]*:\([0-9]*\).*/\1/p' "$listen_log" | head -n 1)"
    pin_to "$CPU_A"
    "${PIN[@]}" "$B/apps/exchange_server" --listen 127.0.0.1:0 --md "127.0.0.1:$md_port" \
        --session EVALS --symbols 16 "$@" >"$server_log" 2>&1 &
    local server_pid=$!
    if ! wait_for "$server_log" "^listening on" "$server_pid"; then
        wait "$server_pid"
        local status=$?
        cat "$server_log"
        kill "$listen_pid" 2>/dev/null
        wait "$listen_pid" 2>/dev/null
        [[ "$status" -eq 5 ]] && { echo "($label: this transport cannot run here)"; return 0; }
        return 1
    fi
    local port
    port="$(sed -n 's/^listening on [0-9.]*:\([0-9]*\).*/\1/p' "$server_log" | head -n 1)"
    local pinned="not pinned (no taskset)"
    command -v taskset >/dev/null && pinned="pinned with taskset"
    echo "$label: exchange_server on CPU $CPU_A and md_listen on CPU $CPU_C, $pinned; load_gen on CPU $CPU_B" \
        >>"$dir/pinning.txt"
    run "$B/apps/load_gen" --connect "127.0.0.1:$port" --connections 16 --cpu "$CPU_B" \
        --rate "$rates" --seconds "$seconds" --warmup 1 --symbols 16 --live 2000 \
        --json "$dir/$label.json" --label "$label" >"$dir/${label}_load_gen.txt" 2>&1
    local load_status=$?
    kill -TERM "$server_pid" 2>/dev/null
    wait_or_kill "$server_pid" 30
    local server_status=$?
    wait "$listen_pid"
    local listen_status=$?
    tail -n 20 "$dir/${label}_load_gen.txt"
    echo "load_gen $load_status, exchange_server $server_status, md_listen $listen_status (2 = a gap)"
    [[ "$load_status" -eq 0 && "$server_status" -eq 0 && "$listen_status" -eq 0 ]]
}

stage_gateway() {
    need_build || return 1
    local failed=0
    gateway_curve "$1" epoll_reference --io epoll --engine reference || failed=1
    gateway_curve "$1" epoll_pooled --io epoll --engine pooled || failed=1
    gateway_curve "$1" uring_pooled --io uring --engine pooled || failed=1
    local curves=()
    for label in epoll_reference epoll_pooled uring_pooled; do
        [[ -f "$1/$label.json" ]] && curves+=("$1/$label.json")
    done
    if [[ "${#curves[@]}" -gt 0 ]]; then
        python3 "$REPO/scripts/latency_curve.py" "${curves[@]}" --plot "$1/latency_curve.png" |
            tee "$1/latency_curve.md"
    fi
    return "$failed"
}

stage_transports() {
    need_build || return 1
    local requests=200000
    [[ "$QUICK" -eq 1 ]] && requests=20000
    run "$B/bench/transport_bench" --cpus "$CPU_A,$CPU_B" --runs "$RUNS" --requests "$requests" \
        --json "$1/transports.json" | tee "$1/transports.txt"
    return "${PIPESTATUS[0]}"
}

stage_sim() {
    need_build && need_data || return 1
    local failed=0
    local common=(--symbol "$SYMBOL" --from "$SIM_FROM" --to "$SIM_TO" --latency-us 50)
    run "$B/apps/mm_sim" "$F" "${common[@]}" --strategy fixed --half-spread 1 \
        --json "$1/fixed.json" >"$1/fixed.txt" 2>&1 || failed=1
    run "$B/apps/mm_sim" "$F" "${common[@]}" --strategy join --json "$1/join.json" \
        >"$1/join.txt" 2>&1 || failed=1
    run "$B/apps/mm_sim" "$F" "${common[@]}" --strategy as --gamma 0.001 --k 200 \
        --json "$1/as.json" >"$1/as.txt" 2>&1 || failed=1
    tail -n 40 "$1/as.txt"
    return "$failed"
}

stage_journal() {
    need_build || return 1
    local failed=0
    run "$REPO/scripts/journal_crash_test.sh" "$B/apps/engine_journal" "$WORK/journal_crash" \
        >"$1/crash_test.txt" 2>&1 || failed=1
    tail -n 10 "$1/crash_test.txt"
    mkdir -p "$WORK/journal_bench"
    run "$B/bench/journal_bench" --dir "$WORK/journal_bench" --cpu "$CPU_A" --runs "$RUNS" \
        --json "$1/journal.json" | tee "$1/journal.txt"
    [[ "${PIPESTATUS[0]}" -eq 0 ]] || failed=1
    {
        echo "journal directory  $WORK/journal_bench"
        df -hT "$WORK/journal_bench" | tail -n 1
    } | tee "$1/disk.txt"
    return "$failed"
}

stage_ticks() {
    need_build && need_data || return 1
    local failed=0 store="$WORK/ticks"
    mkdir -p "$store"
    run "$B/apps/tick_store" profile "$F" >"$1/profile.txt" 2>&1 || failed=1
    for codec in raw delta; do
        local t0 t1
        t0=$(date +%s%N)
        run "$B/apps/tick_store" record --codec "$codec" "$F" "$store/day.$codec.ticks" \
            >"$1/record_$codec.txt" 2>&1 || failed=1
        t1=$(date +%s%N)
        echo "record milliseconds $(((t1 - t0) / 1000000))" >>"$1/record_$codec.txt"
        run "$B/apps/tick_store" info "$store/day.$codec.ticks" >"$1/info_$codec.txt" 2>&1 || failed=1
        run "$B/apps/tick_store" verify --against "$F" "$store/day.$codec.ticks" \
            >"$1/verify_$codec.txt" 2>&1 || failed=1
        tail -n 3 "$1/verify_$codec.txt"
    done
    for codec in raw delta; do
        printf '%-6s %15s bytes\n' "$codec" "$(stat -c %s "$store/day.$codec.ticks" 2>/dev/null || echo missing)"
    done | tee "$1/sizes.txt"
    if command -v zstd >/dev/null; then
        for level in 3 19; do
            local t0 t1
            t0=$(date +%s%N)
            zstd -q -f -T0 "-$level" "$store/day.raw.ticks" -o "$store/day.raw.ticks.zst$level" ||
                failed=1
            t1=$(date +%s%N)
            echo "zstd -$level -T0: $(stat -c %s "$store/day.raw.ticks.zst$level") bytes in $(((t1 - t0) / 1000000)) ms" |
                tee -a "$1/zstd.txt"
        done
    else
        echo "zstd is not installed: no general-purpose comparison" | tee "$1/zstd.txt"
    fi
    local max=20000000
    [[ "$QUICK" -eq 1 ]] && max=2000000
    run "$B/bench/tick_bench" --file "$F" --max-ticks "$max" --cpu "$CPU_A" --runs "$RUNS" \
        --json "$1/tick_bench.json" >"$1/tick_bench.txt" 2>&1 || failed=1
    tail -n 25 "$1/tick_bench.txt"
    return "$failed"
}

stage_summary() {
    python3 "$REPO/scripts/summarize_evals.py" "$RESULTS" | tee "$RESULTS/SUMMARY.md"
    return "${PIPESTATUS[0]}"
}

# --- the run ----------------------------------------------------------------

load_settings
echo "results:   $RESULTS"
echo "work:      $WORK"
echo "cpus:      $CPUS"
echo "runs:      $RUNS$([[ "$QUICK" -eq 1 ]] && echo ' (quick)')"

for stage in "${STAGES[@]}"; do
    selected "$stage" || continue
    dir="$RESULTS/$stage"
    mkdir -p "$dir"
    say "$stage: $(describe "$stage")"
    start=$(date +%s)
    "stage_$stage" "$dir" 2>&1 | tee "$dir/log.txt"
    status="${PIPESTATUS[0]}"
    load_settings
    seconds=$(($(date +%s) - start))
    record "$stage" "$status" "$seconds"
    if [[ "$status" -eq 0 ]]; then
        echo "-- $stage: ok (${seconds} s)"
    else
        echo "-- $stage: FAILED with status $status (${seconds} s); see $dir/log.txt"
        if [[ "$stage" == "build" || "$stage" == "data" ]]; then
            echo "Nothing else can run without it. Stopping." >&2
            exit 1
        fi
    fi
done

echo
echo "Done. Results in $RESULTS"
echo "The status of every stage is in $STATUS; the summary in $RESULTS/SUMMARY.md."
