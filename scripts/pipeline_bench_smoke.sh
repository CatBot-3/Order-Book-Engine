#!/usr/bin/env bash
# Run pipeline_bench once on a small generated file and check that it produces
# a report and valid JSON, and that the pipeline's output matched the single
# thread's. A smoke test of the harness, not a measurement.
#
#   scripts/pipeline_bench_smoke.sh <name> <flow_gen> <pipeline_bench> <workdir> [pipeline_bench options...]
#
# <name> labels the temporary files.
set -euo pipefail

NAME="$1"
FLOW_GEN="$2"
BENCH="$3"
WORKDIR="$4"
shift 4
FILE="$WORKDIR/pipeline_bench_smoke_$NAME.itch"
JSON="$WORKDIR/pipeline_bench_smoke_$NAME.json"

"$FLOW_GEN" --seed 5 --commands 150000 --symbols 40 --live 4000 --no-verify "$FILE" >/dev/null
"$BENCH" "$FILE" --runs 2 --warmup 1 --json "$JSON" --label smoke "$@"

grep -q '"pipeline_over_single"' "$JSON"
grep -q '"matches_single_thread": true' "$JSON"
if command -v python3 >/dev/null; then
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert len(d['runs']) == 2 and len(d['single_runs']) == 2 and d['messages'] > 100000" "$JSON"
fi
echo "pipeline bench smoke ok ($NAME)"
