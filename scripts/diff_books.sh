#!/usr/bin/env bash
# Success criterion 3: every optimized book must publish the same
# best-bid-and-offer stream as the reference book, security by security.
#
# On a real file:
#   scripts/diff_books.sh --replay build/release/apps/book_replay --file data/01302019.NASDAQ_ITCH50
#   scripts/diff_books.sh --replay ... --file ... --impl flat-store --reserve 3000000
#
# On a generated file (what the test suite runs):
#   scripts/diff_books.sh --replay <book_replay> --synth <itch_synth>
#
# With no --impl, every implementation `book_replay --list` names is checked.
# Exit status: 0 all identical, 1 a difference or an error, 4 an implementation
# is not written yet.
set -euo pipefail

REPLAY=""
SYNTH=""
FILE=""
RESERVE=""
WORKDIR="${TMPDIR:-/tmp}"
IMPLS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --replay) REPLAY="$2"; shift 2 ;;
        --synth) SYNTH="$2"; shift 2 ;;
        --file) FILE="$2"; shift 2 ;;
        --impl) IMPLS+=("$2"); shift 2 ;;
        --reserve) RESERVE="$2"; shift 2 ;;
        --workdir) WORKDIR="$2"; shift 2 ;;
        -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; exit 1 ;;
    esac
done

if [[ -z "$REPLAY" || ( -z "$SYNTH" && -z "$FILE" ) ]]; then
    echo "usage: diff_books.sh --replay <book_replay> (--file <itch file> | --synth <itch_synth>) [--impl NAME]... [--reserve N]" >&2
    exit 1
fi

if [[ -z "$FILE" ]]; then
    FILE="$WORKDIR/diff_books_synthetic.itch"
    "$SYNTH" --seed 20261004 --messages 400000 --symbols 300 --live 30000 "$FILE" >/dev/null
fi

if [[ "${#IMPLS[@]}" -eq 0 ]]; then
    mapfile -t IMPLS < <("$REPLAY" --list | awk '{print $1}' | grep -v '^reference$')
fi

extra=()
if [[ -n "$RESERVE" ]]; then
    extra=(--reserve "$RESERVE")
fi

ref_hashes="$WORKDIR/diff_books_reference.txt"
echo "reference: replaying $FILE"
"$REPLAY" --impl reference --hashes "$ref_hashes" "$FILE" >"$WORKDIR/diff_books_reference.log"
grep -E '^(RESULT|best bid/offer stream hash)' "$WORKDIR/diff_books_reference.log" | sed 's/^/  /'

worst=0
for impl in "${IMPLS[@]}"; do
    hashes="$WORKDIR/diff_books_$impl.txt"
    log="$WORKDIR/diff_books_$impl.log"
    echo "$impl: replaying"
    set +e
    "$REPLAY" --impl "$impl" "${extra[@]}" --hashes "$hashes" "$FILE" >"$log" 2>&1
    status=$?
    set -e
    if [[ "$status" -eq 4 ]]; then
        echo "  not written yet: $(grep -m1 'not built yet' "$log" | sed 's/^not built yet: //')"
        [[ "$worst" -eq 0 ]] && worst=4
        continue
    fi
    if [[ "$status" -ne 0 ]]; then
        echo "  FAILED: book_replay exited with status $status"
        sed 's/^/    /' "$log" | tail -n 12
        worst=1
        continue
    fi
    if diff -q "$ref_hashes" "$hashes" >/dev/null; then
        echo "  identical to the reference for all $(wc -l <"$hashes") securities"
    else
        differing="$(diff "$ref_hashes" "$hashes" | grep -c '^<' || true)"
        echo "  DIFFERS from the reference for $differing securities. The first few"
        echo "  (columns: symbol, locate, updates published, hash of the update stream):"
        awk -v impl="$impl" '
            NR == FNR { ref[$1] = $0; next }
            ref[$1] != $0 {
                printf "    reference      %s\n    %-14s %s\n", ref[$1], impl, $0
                if (++shown == 3) exit
            }' "$ref_hashes" "$hashes"
        echo "  A different update count means the books diverged in what they published;"
        echo "  the same count with a different hash means a price, size or timestamp differs."
        echo "  book_view <file> <symbol> --at <time> shows the reference book at a moment."
        worst=1
    fi
done

exit "$worst"
