#!/usr/bin/env bash
# Every matching engine must behave exactly like the reference engine: the
# same market data, byte for byte, and the same reports to owners, in the same
# order. flow_gen prints a hash of each; this compares them.
#
#   scripts/diff_engines.sh --flow-gen build/release/apps/flow_gen
#   scripts/diff_engines.sh --flow-gen ... --engine pooled -- --commands 50000000 --symbols 2000
#
# With no --engine, every engine `flow_gen --list` names is checked. Anything
# after "--" is passed to flow_gen, for both runs.
# Exit status: 0 all identical, 1 a difference or an error, 4 an engine is not
# written yet.
set -euo pipefail

FLOW_GEN=""
ENGINES=()
OPTIONS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --flow-gen) FLOW_GEN="$2"; shift 2 ;;
        --engine) ENGINES+=("$2"); shift 2 ;;
        --) shift; OPTIONS=("$@"); break ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 1 ;;
    esac
done

if [[ -z "$FLOW_GEN" ]]; then
    echo "usage: diff_engines.sh --flow-gen <flow_gen> [--engine NAME]... [-- flow_gen options]" >&2
    exit 1
fi
if [[ "${#OPTIONS[@]}" -eq 0 ]]; then
    OPTIONS=(--seed 20261004 --commands 500000 --symbols 100 --live 20000)
fi
if [[ "${#ENGINES[@]}" -eq 0 ]]; then
    mapfile -t ENGINES < <("$FLOW_GEN" --list | awk '{print $1}' | grep -v '^reference$')
fi

hashes() { grep -E "^(feed|report) hash:" <<<"$1"; }

echo "reference: flow_gen ${OPTIONS[*]}"
reference="$("$FLOW_GEN" --engine reference "${OPTIONS[@]}")"
hashes "$reference" | sed 's/^/  /'

worst=0
for name in "${ENGINES[@]}"; do
    echo "$name:"
    set +e
    output="$("$FLOW_GEN" --engine "$name" "${OPTIONS[@]}" 2>&1)"
    status=$?
    set -e
    if [[ "$status" -eq 4 ]]; then
        echo "  not written yet: $(grep -m1 'not built yet' <<<"$output" | sed 's/^not built yet: //')"
        [[ "$worst" -eq 0 ]] && worst=4
        continue
    fi
    if [[ "$status" -ne 0 ]]; then
        echo "  FAILED: flow_gen exited with status $status"
        sed 's/^/    /' <<<"$output" | tail -n 12
        worst=1
        continue
    fi
    if [[ "$(hashes "$output")" == "$(hashes "$reference")" ]]; then
        echo "  identical to the reference"
    else
        echo "  DIFFERS from the reference:"
        hashes "$output" | sed 's/^/    /'
        echo "  A different feed hash means the market was told something different; a"
        echo "  different report hash with the same feed hash means only the owners were."
        echo "  tests/engine/engine_differential_test.cpp finds the first request where the"
        echo "  two engines disagree."
        worst=1
    fi
done

exit "$worst"
