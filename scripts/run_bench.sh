#!/usr/bin/env bash
# Run replay_bench the standard way and keep the output.
#
#   scripts/run_bench.sh <itch file> [label] [extra replay_bench options...]
#
#   scripts/run_bench.sh data/01302019.NASDAQ_ITCH50 baseline
#   scripts/run_bench.sh data/01302019.NASDAQ_ITCH50 flat-table --runs 7
#   CPU=3 scripts/run_bench.sh data/01302019.NASDAQ_ITCH50 baseline
#   PERF_STAT=1 scripts/run_bench.sh data/01302019.NASDAQ_ITCH50 baseline
#
# Environment:
#   BUILD      build directory (default: build/release)
#   CPU        CPU to pin to (default: 2, away from CPU 0 where interrupts land)
#   PERF_STAT  if set, also run once under `perf stat` as a whole-process
#              cross-check of the in-process counters
#
# Results go to results/<utc time>-<label>/ (ignored by git): the text report,
# the JSON, and the commit the binary was built from. Copy the numbers you keep
# into docs/optimization-log.md.
set -euo pipefail

if [[ $# -lt 1 ]]; then
    sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi

FILE="$1"
LABEL="${2:-run}"
shift
[[ $# -gt 0 ]] && shift

cd "$(dirname "$0")/.."
BUILD="${BUILD:-build/release}"
CPU="${CPU:-2}"
BENCH="$BUILD/bench/replay_bench"

if [[ ! -x "$BENCH" ]]; then
    echo "error: $BENCH not found. Build it first:" >&2
    echo "  cmake --preset release && cmake --build --preset release" >&2
    exit 1
fi

OUT="results/$(date -u +%Y%m%dT%H%M%SZ)-${LABEL}"
mkdir -p "$OUT"

{
    echo "commit: $(git rev-parse HEAD 2>/dev/null || echo 'not a git checkout')"
    if ! git diff --quiet 2>/dev/null; then
        echo "tree:   has uncommitted changes (the numbers do not belong to that commit alone)"
    fi
    echo "date:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host:   $(uname -srm)"
} | tee "$OUT/provenance.txt"
echo

set +e
"$BENCH" "$FILE" --cpu "$CPU" --label "$LABEL" --json "$OUT/result.json" "$@" | tee "$OUT/report.txt"
status="${PIPESTATUS[0]}"
set -e
if [[ "$status" -ne 0 ]]; then
    echo "replay_bench exited with status $status" >&2
    exit "$status"
fi

if [[ -n "${PERF_STAT:-}" ]]; then
    if command -v perf >/dev/null; then
        echo
        echo "== perf stat, whole process (includes loading the file; one measured run) =="
        perf stat -e cycles,instructions,cache-misses,branch-misses -o "$OUT/perf_stat.txt" \
            "$BENCH" "$FILE" --cpu "$CPU" --runs 1 --warmup 0 "$@" >/dev/null || true
        cat "$OUT/perf_stat.txt"
    else
        echo "perf is not installed; skipping PERF_STAT" >&2
    fi
fi

echo
echo "saved to $OUT"
