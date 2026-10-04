#!/usr/bin/env bash
# Success criterion 7 through the real apps: run seeded order flow through a
# matching engine, write the market data it publishes as an ITCH file, and
# check the result three ways.
#
#   1. flow_gen's own round trip must pass: the published bytes, parsed back
#      through the feed handler, rebuild the engine's book.
#   2. book_replay, the phase 2 tool that knows nothing about the engine, must
#      replay the file with every invariant at zero, and must never see a
#      locked or crossed best bid and offer.
#   3. A second run with the same options must print the same hashes.
#
#   scripts/flow_round_trip.sh <flow_gen> <book_replay> [workdir] [flow_gen options...]
#
# Exit status: 0 all three hold, 1 one of them does not, 4 the engine named
# with --engine is not written yet.
set -euo pipefail

FLOW_GEN="$1"
REPLAY="$2"
WORKDIR="${3:-${TMPDIR:-/tmp}}"
shift $(( $# < 3 ? $# : 3 ))
FILE="$WORKDIR/flow_round_trip.itch"

run_flow() {
    "$FLOW_GEN" --seed 20261004 --commands 400000 --symbols 100 --live 20000 "$@"
}

set +e
run_flow "$@" "$FILE" >"$WORKDIR/flow_round_trip_1.txt" 2>&1
status=$?
set -e
cat "$WORKDIR/flow_round_trip_1.txt"
if [[ "$status" -eq 4 ]]; then
    exit 4
fi
if [[ "$status" -ne 0 ]]; then
    echo "error: flow_gen exited with status $status" >&2
    exit 1
fi

echo
"$REPLAY" "$FILE" >"$WORKDIR/flow_round_trip_replay.txt"
grep -E "^RESULT|locked|crossed" "$WORKDIR/flow_round_trip_replay.txt"
if ! grep -q "^RESULT: clean" "$WORKDIR/flow_round_trip_replay.txt"; then
    echo "error: book_replay did not replay the engine's feed cleanly" >&2
    exit 1
fi
# The four "locked, ..." and "crossed, ..." lines must all end in 0.
if grep -E "^  (locked|crossed), " "$WORKDIR/flow_round_trip_replay.txt" | grep -qv " 0$"; then
    echo "error: the engine's feed shows a locked or crossed book" >&2
    exit 1
fi

run_flow "$@" >"$WORKDIR/flow_round_trip_2.txt"
if ! diff <(grep "hash:" "$WORKDIR/flow_round_trip_1.txt") \
          <(grep "hash:" "$WORKDIR/flow_round_trip_2.txt") >/dev/null; then
    echo "error: two runs with the same options printed different hashes" >&2
    exit 1
fi
echo "two runs agree"
