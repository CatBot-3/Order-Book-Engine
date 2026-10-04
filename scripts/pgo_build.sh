#!/usr/bin/env bash
# Build replay_bench with profile-guided optimization (phase 4, experiment 10).
#
#   scripts/pgo_build.sh <training itch file> [replay_bench options for the training run]
#
#   scripts/pgo_build.sh data/01302019.NASDAQ_ITCH50
#   scripts/pgo_build.sh data/01302019.NASDAQ_ITCH50 --impl flat-store --reserve 3000000
#   CXX=clang++ scripts/pgo_build.sh data/01302019.NASDAQ_ITCH50
#
# Three steps, all in build/release-pgo:
#   1. build an instrumented replay_bench;
#   2. run it on the training file, which writes the profile;
#   3. rebuild with the profile.
# The result is build/release-pgo/bench/replay_bench. Benchmark it with
#   BUILD=build/release-pgo scripts/run_bench.sh <file> pgo
#
# Be honest about the training data in the log entry. Training on the same file
# you then measure is the best case for PGO. Training on one day and measuring
# another is the result that would hold in practice.
set -euo pipefail

if [[ $# -lt 1 ]]; then
    sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
TRAIN="$1"
shift

cd "$(dirname "$0")/.."
BUILD="build/release-pgo"
PROFILES="$PWD/$BUILD/pgo-profiles"

echo "==> 1/3 instrumented build"
# Start from nothing. A build directory remembers its compiler, and profiles
# left over from an earlier run would be mixed into this one.
rm -rf "$BUILD"
mkdir -p "$PROFILES"
cmake --preset release-pgo -DOBE_PGO=generate -DOBE_PGO_DIR="$PROFILES" >/dev/null
cmake --build --preset release-pgo --target replay_bench

echo "==> 2/3 training run on $TRAIN"
"$BUILD/bench/replay_bench" "$TRAIN" --runs 1 --warmup 0 --no-verify "$@" >/dev/null

# Clang writes raw profiles that have to be merged before they can be used.
if compgen -G "$PROFILES/*.profraw" >/dev/null; then
    MERGE="${LLVM_PROFDATA:-}"
    if [[ -z "$MERGE" ]]; then
        MERGE="$(command -v llvm-profdata || compgen -c llvm-profdata- | sort -V | tail -n1 || true)"
    fi
    if [[ -z "$MERGE" ]]; then
        echo "error: llvm-profdata not found. Set LLVM_PROFDATA to its path." >&2
        exit 1
    fi
    "$MERGE" merge -o "$PROFILES/merged.profdata" "$PROFILES"/*.profraw
fi
if [[ -z "$(find "$PROFILES" -type f | head -n1)" ]]; then
    echo "error: the training run wrote no profile into $PROFILES" >&2
    exit 1
fi

echo "==> 3/3 optimized build"
cmake --preset release-pgo -DOBE_PGO=use -DOBE_PGO_DIR="$PROFILES" >/dev/null
cmake --build --preset release-pgo --target replay_bench

echo "done: $BUILD/bench/replay_bench"
