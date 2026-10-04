#!/usr/bin/env bash
# Success criterion 8, in miniature, through the real programs: start the
# exchange, a market-data subscriber and the load generator on loopback, and
# check that the whole loop held together.
#
#   orders:       load_gen ──TCP──> exchange_server
#   market data:  exchange_server ──UDP──> md_listen
#
# It checks that every request was acknowledged, that the subscriber saw the
# whole feed with no sequence gap and rebuilt a consistent book, and that the
# server shut down cleanly. It is a test that the pieces work together, not a
# measurement: the rates are low and the run is short.
#
#   scripts/gateway_smoke.sh <exchange_server> <load_gen> <md_listen> [workdir] [exchange_server options...]
#
# Exit status: 0 all of that held, 1 something did not, 4 the engine named
# with --engine is not written yet.
set -uo pipefail

SERVER="$1"
LOAD_GEN="$2"
LISTEN="$3"
WORKDIR="${4:-${TMPDIR:-/tmp}}"
shift $(( $# < 4 ? $# : 4 ))

SERVER_LOG="$WORKDIR/gateway_smoke_server.txt"
LISTEN_LOG="$WORKDIR/gateway_smoke_listen.txt"
JSON="$WORKDIR/gateway_smoke.json"
SERVER_PID=""
LISTEN_PID=""

cleanup() {
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null
    [[ -n "$LISTEN_PID" ]] && kill "$LISTEN_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

# Waits for a line matching $2 to appear in file $1, for up to ten seconds, or
# until process $3 has exited without printing it.
wait_for() {
    for _ in $(seq 1 100); do
        grep -q "$2" "$1" 2>/dev/null && return 0
        kill -0 "$3" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

# The subscriber first, on a port the system picks, so that it is listening
# before the exchange publishes its first packet. An ordinary loopback address
# is used so this runs in containers where multicast is not available.
"$LISTEN" --md 127.0.0.1:0 --session SMOKE >"$LISTEN_LOG" 2>&1 &
LISTEN_PID=$!
if ! wait_for "$LISTEN_LOG" "^listening on" "$LISTEN_PID"; then
    echo "error: md_listen did not start" >&2
    cat "$LISTEN_LOG" >&2
    exit 1
fi
MD_PORT="$(sed -n 's/^listening on [0-9.]*:\([0-9]*\).*/\1/p' "$LISTEN_LOG" | head -n 1)"

"$SERVER" --listen 127.0.0.1:0 --md "127.0.0.1:$MD_PORT" --session SMOKE --symbols 8 "$@" \
    >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
if ! wait_for "$SERVER_LOG" "^listening on" "$SERVER_PID"; then
    wait "$SERVER_PID"
    status=$?
    cat "$SERVER_LOG" >&2
    SERVER_PID=""
    if [[ "$status" -eq 4 ]]; then
        exit 4
    fi
    echo "error: exchange_server did not start" >&2
    exit 1
fi
PORT="$(sed -n 's/^listening on [0-9.]*:\([0-9]*\).*/\1/p' "$SERVER_LOG" | head -n 1)"

"$LOAD_GEN" --connect "127.0.0.1:$PORT" --connections 4 --rate 2000,5000 --seconds 1 \
    --warmup 1 --symbols 8 --live 300 --json "$JSON" --label smoke
load_status=$?

# Stop the exchange. It announces the end of the session, which is what lets
# the subscriber finish by itself.
kill -TERM "$SERVER_PID"
wait "$SERVER_PID"
server_status=$?
SERVER_PID=""
wait "$LISTEN_PID"
listen_status=$?
LISTEN_PID=""

echo
cat "$SERVER_LOG"
cat "$LISTEN_LOG"

fail=0
if [[ "$load_status" -ne 0 ]]; then
    echo "error: load_gen exited with status $load_status" >&2
    fail=1
fi
if [[ "$server_status" -ne 0 ]]; then
    echo "error: exchange_server exited with status $server_status" >&2
    fail=1
fi
if [[ "$listen_status" -ne 0 ]]; then
    echo "error: md_listen exited with status $listen_status (2 means a gap in the feed)" >&2
    fail=1
fi
if ! grep -q '"unanswered": 0' "$JSON"; then
    echo "error: some requests were never acknowledged" >&2
    fail=1
fi
if command -v python3 >/dev/null; then
    python3 - "$JSON" <<'EOF' || fail=1
import json, sys
d = json.load(open(sys.argv[1]))
assert len(d["rates"]) == 2, d
for r in d["rates"]:
    assert r["unanswered"] == 0, r
    assert r["acknowledged"] == r["sent"] > 0, r
    assert r["sent"] == r["target_rate"], r  # one measured second at each rate
EOF
fi
if [[ "$fail" -eq 0 ]]; then
    echo "gateway smoke ok"
fi
exit "$fail"
