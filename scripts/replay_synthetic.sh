#!/usr/bin/env bash
# Generate a synthetic day and replay it through book_replay twice. The replay
# must be clean (exit status 0) and both runs must print the same hash.
#
#   scripts/replay_synthetic.sh <itch_synth> <book_replay> [workdir]
set -euo pipefail

SYNTH="$1"
REPLAY="$2"
WORKDIR="${3:-${TMPDIR:-/tmp}}"
FILE="$WORKDIR/replay_synthetic.itch"

"$SYNTH" --seed 20261003 --messages 500000 --symbols 200 --live 20000 "$FILE" >/dev/null

"$REPLAY" "$FILE" | tee "$WORKDIR/replay_synthetic_1.txt"
"$REPLAY" "$FILE" >"$WORKDIR/replay_synthetic_2.txt"

if ! diff -q "$WORKDIR/replay_synthetic_1.txt" "$WORKDIR/replay_synthetic_2.txt" >/dev/null; then
    echo "error: two replays of the same file printed different results" >&2
    exit 1
fi
echo "two replays agree"
