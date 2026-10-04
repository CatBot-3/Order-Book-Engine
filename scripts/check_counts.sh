#!/usr/bin/env bash
# Success criterion 1: per-type message counts from the C++ parser must equal
# the counts from the independent Python counter.
#
# On a real file (takes several minutes; the Python side is the slow one):
#   scripts/check_counts.sh --stats build/release/apps/itch_stats --file data/01302019.NASDAQ_ITCH50
#
# On a generated file (what CI runs):
#   scripts/check_counts.sh --stats <itch_stats> --synth <itch_synth>
set -euo pipefail

STATS=""
SYNTH=""
FILE=""
PYTHON="python3"
WORKDIR="${TMPDIR:-/tmp}"
HERE="$(cd "$(dirname "$0")" && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stats) STATS="$2"; shift 2 ;;
        --synth) SYNTH="$2"; shift 2 ;;
        --file) FILE="$2"; shift 2 ;;
        --python) PYTHON="$2"; shift 2 ;;
        --workdir) WORKDIR="$2"; shift 2 ;;
        -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 1 ;;
    esac
done

if [[ -z "$STATS" || ( -z "$SYNTH" && -z "$FILE" ) ]]; then
    echo "usage: check_counts.sh --stats <itch_stats> (--file <itch file> | --synth <itch_synth>)" >&2
    exit 1
fi

if [[ -z "$FILE" ]]; then
    FILE="$WORKDIR/check_counts_synthetic.itch"
    "$SYNTH" --seed 20261003 --messages 200000 --symbols 50 "$FILE" >/dev/null
fi

cpp_out="$WORKDIR/check_counts_cpp.txt"
py_out="$WORKDIR/check_counts_py.txt"

"$STATS" --counts "$FILE" >"$cpp_out"
"$PYTHON" "$HERE/count_messages.py" "$FILE" >"$py_out"

if diff -u "$py_out" "$cpp_out"; then
    echo "counts match: $(tail -n1 "$cpp_out") across $(($(wc -l <"$cpp_out") - 1)) message types"
else
    echo "error: C++ and Python counts differ (Python is '-', C++ is '+')" >&2
    exit 1
fi
