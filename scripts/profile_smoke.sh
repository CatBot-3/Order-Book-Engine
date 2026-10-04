#!/usr/bin/env bash
# Run feed_profile on a small generated file and check that its figures agree
# with book_replay on the same file.
#
#   scripts/profile_smoke.sh <itch_synth> <feed_profile> <book_replay> [workdir]
set -euo pipefail

SYNTH="$1"
PROFILE="$2"
REPLAY="$3"
WORKDIR="${4:-${TMPDIR:-/tmp}}"
FILE="$WORKDIR/profile_smoke.itch"
OUT="$WORKDIR/profile_smoke.txt"

"$SYNTH" --seed 5 --messages 300000 --symbols 60 --live 8000 "$FILE" >/dev/null
"$PROFILE" "$FILE" | tee "$OUT"

for section in "^orders" "^order reference numbers" "^price levels on a side" \
               "^where updates land" "^securities" "^self-check: .*: ok"; do
    grep -Eq "$section" "$OUT" || { echo "error: missing '$section' in the profile" >&2; exit 1; }
done

number() { grep -E "$1" "$2" | head -n1 | grep -oE '[0-9][0-9,]*' | tail -n1 | tr -d ','; }

profile_resting="$(number 'resting at the end' "$OUT")"
replay_resting="$("$REPLAY" "$FILE" | grep -E 'orders resting at the end' | grep -oE '[0-9][0-9,]*' | tr -d ',')"
if [[ "$profile_resting" != "$replay_resting" ]]; then
    echo "error: feed_profile says $profile_resting orders rest at the end, book_replay says $replay_resting" >&2
    exit 1
fi
echo "profile smoke ok ($profile_resting orders resting at the end, matching book_replay)"
