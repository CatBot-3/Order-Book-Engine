#!/usr/bin/env bash
# Run replay_bench once on a small generated file and check that it produces a
# report and valid JSON. A smoke test of the harness, not a measurement.
#
#   scripts/bench_smoke.sh <parse|book> <itch_synth> <replay_bench> [workdir]
set -euo pipefail

HANDLER="$1"
SYNTH="$2"
BENCH="$3"
WORKDIR="${4:-${TMPDIR:-/tmp}}"
FILE="$WORKDIR/bench_smoke_$HANDLER.itch"
JSON="$WORKDIR/bench_smoke_$HANDLER.json"

"$SYNTH" --seed 3 --messages 200000 --symbols 50 --live 5000 "$FILE" >/dev/null
"$BENCH" "$FILE" --handler "$HANDLER" --runs 2 --warmup 1 --json "$JSON" --label smoke

grep -q '"median_msgs_per_s"' "$JSON"
grep -q "\"workload\": \"$HANDLER\"" "$JSON"
if command -v python3 >/dev/null; then
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert len(d['runs']) == 2 and d['messages'] > 200000" "$JSON"
fi
echo "bench smoke ok ($HANDLER)"
