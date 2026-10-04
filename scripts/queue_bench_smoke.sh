#!/usr/bin/env bash
# Run queue_bench once on a short transfer and check that it produces a report
# and valid JSON, and that every item arrived in order. A smoke test of the
# harness, not a measurement.
#
#   scripts/queue_bench_smoke.sh <name> <queue_bench> <workdir> [queue_bench options...]
#
# <name> labels the temporary file.
set -euo pipefail

NAME="$1"
BENCH="$2"
WORKDIR="$3"
shift 3
JSON="$WORKDIR/queue_bench_smoke_$NAME.json"

"$BENCH" --items 300000 --capacity 1024 --runs 2 --warmup 1 --json "$JSON" --label smoke "$@"

grep -q '"median_msgs_per_s"' "$JSON"
grep -q '"in_order": true' "$JSON"
if command -v python3 >/dev/null; then
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert len(d['runs']) == 2 and all(r['received'] == 300000 and r['out_of_order'] == 0 for r in d['runs'])" "$JSON"
fi
echo "queue bench smoke ok ($NAME)"
