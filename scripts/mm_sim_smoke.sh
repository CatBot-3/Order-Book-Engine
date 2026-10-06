#!/usr/bin/env bash
# The market-making simulator through the real programs: generate a market
# with flow_gen, then run mm_sim over it and check what must hold whatever the
# numbers are.
#
#   1. A strategy that never quotes trades nothing and makes nothing.
#   2. The finished strategies run clean, with and without latency, and are
#      filled at least once: the report's profit figures must agree with each
#      other (mm_sim checks that itself and says so in its RESULT line).
#   3. The same run twice writes the same report, byte for byte.
#   4. A symbol that is not in the file is an error, not an empty report.
#
#   scripts/mm_sim_smoke.sh <flow_gen> <mm_sim> [workdir] [mm_sim options...]
#
# With extra mm_sim options (for example "--strategy as") only that one run is
# made, and its exit status is passed on.
#
# This checks that the simulator holds together. It says nothing about any
# strategy: the market is generated, and the numbers mean nothing.
#
# Exit status: 0 all of that held, 1 something did not, 4 the strategy named
# is not written yet.
set -uo pipefail

FLOW_GEN="$1"
MM_SIM="$2"
WORKDIR="${3:-${TMPDIR:-/tmp}}"
shift $(( $# < 3 ? $# : 3 ))
mkdir -p "$WORKDIR"
FILE="$WORKDIR/mm_sim_smoke.itch"
SYMBOL="S00001"

fail() {
    echo "error: $*" >&2
    exit 1
}

"$FLOW_GEN" --seed 20261004 --commands 200000 --symbols 4 --live 400 "$FILE" \
    >"$WORKDIR/mm_sim_smoke_flow.txt" 2>&1 || fail "flow_gen failed"

# A number from a report written with --json.
field() {
    python3 - "$1" "$2" <<'EOF'
import json, sys
print(json.load(open(sys.argv[1]))[sys.argv[2]])
EOF
}

# run <name> <mm_sim options...>: runs mm_sim, keeps its output and report.
run() {
    local name="$1"
    shift
    "$MM_SIM" "$FILE" --symbol "$SYMBOL" --json "$WORKDIR/mm_sim_smoke_$name.json" "$@" \
        >"$WORKDIR/mm_sim_smoke_$name.txt" 2>&1
}

if [[ $# -gt 0 ]]; then
    run extra "$@"
    status=$?
    cat "$WORKDIR/mm_sim_smoke_extra.txt"
    if [[ "$status" -eq 4 ]]; then
        exit 4
    fi
    [[ "$status" -eq 0 ]] || fail "mm_sim $* exited with status $status"
    grep -q "^RESULT: consistent" "$WORKDIR/mm_sim_smoke_extra.txt" || fail "no clean result"
    [[ "$(field "$WORKDIR/mm_sim_smoke_extra.json" fills)" != "0" ]] ||
        fail "never filled: the check is vacuous (try parameters that quote nearer the mid)"
    echo "mm_sim smoke ok ($*)"
    exit 0
fi

# 1. Never quoting.
run none --strategy none || fail "the 'none' strategy did not run clean"
[[ "$(field "$WORKDIR/mm_sim_smoke_none.json" fills)" == "0" ]] || fail "'none' was filled"
[[ "$(field "$WORKDIR/mm_sim_smoke_none.json" placed)" == "0" ]] || fail "'none' placed a quote"
[[ "$(field "$WORKDIR/mm_sim_smoke_none.json" profit)" == "0.0" ]] || fail "'none' made money"
[[ "$(field "$WORKDIR/mm_sim_smoke_none.json" market_executed)" != "0" ]] ||
    fail "the generated market did not trade"

# 2. The finished strategies.
run fixed --strategy fixed --half-spread 1 || fail "the 'fixed' strategy did not run clean"
run join --strategy join || fail "the 'join' strategy did not run clean"
run late --strategy fixed --half-spread 2 --latency-us 50 --rebate 20 \
    --fills "$WORKDIR/mm_sim_smoke_fills.csv" || fail "the run with latency did not run clean"
for name in fixed join late; do
    grep -q "^RESULT: consistent" "$WORKDIR/mm_sim_smoke_$name.txt" ||
        fail "'$name' did not report a consistent result"
    [[ "$(field "$WORKDIR/mm_sim_smoke_$name.json" consistent)" == "True" ]] ||
        fail "'$name' wrote an inconsistent report"
    [[ "$(field "$WORKDIR/mm_sim_smoke_$name.json" fills)" != "0" ]] ||
        fail "'$name' was never filled: the check is vacuous"
done
fills="$(field "$WORKDIR/mm_sim_smoke_late.json" fills)"
lines="$(($(wc -l <"$WORKDIR/mm_sim_smoke_fills.csv") - 1))"
[[ "$fills" == "$lines" ]] || fail "the report says $fills fills and the CSV holds $lines"

# 3. The same run again.
cp "$WORKDIR/mm_sim_smoke_late.json" "$WORKDIR/mm_sim_smoke_late_first.json"
run late --strategy fixed --half-spread 2 --latency-us 50 --rebate 20 || fail "the rerun failed"
cmp -s "$WORKDIR/mm_sim_smoke_late.json" "$WORKDIR/mm_sim_smoke_late_first.json" ||
    fail "the same run gave a different report"

# 4. A symbol that is not there.
"$MM_SIM" "$FILE" --symbol NOSUCH >"$WORKDIR/mm_sim_smoke_missing.txt" 2>&1
[[ $? -eq 3 ]] || fail "a missing symbol was not reported as one"

cat "$WORKDIR/mm_sim_smoke_fixed.txt"
echo
echo "mm_sim smoke ok"
