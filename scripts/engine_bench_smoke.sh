#!/usr/bin/env bash
# Run engine_bench once on a short tape and check that it produces a report
# and valid JSON. A smoke test of the harness, not a measurement.
#
#   scripts/engine_bench_smoke.sh <name> <engine_bench> <workdir> [engine_bench options...]
#
# <name> labels the temporary file.
set -euo pipefail

NAME="$1"
BENCH="$2"
WORKDIR="$3"
shift 3
JSON="$WORKDIR/engine_bench_smoke_$NAME.json"

"$BENCH" --seed 3 --commands 120000 --symbols 20 --live 3000 --runs 2 --warmup 1 \
    --json "$JSON" --label smoke "$@"

grep -q '"median_msgs_per_s"' "$JSON"
grep -q '"matches_reference": true' "$JSON"
if command -v python3 >/dev/null; then
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert len(d['runs']) == 2 and d['messages'] > 50000 and d['prefill_requests'] > 0" "$JSON"
fi
echo "engine bench smoke ok ($NAME)"
