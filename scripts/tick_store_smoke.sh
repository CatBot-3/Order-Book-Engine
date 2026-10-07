#!/usr/bin/env bash
# The tick store through the real program, on a generated feed: record it,
# check the store against the feed, ask it for ranges, then cut it short and
# damage it and see that it says so.
#
#   scripts/tick_store_smoke.sh <itch_synth> <tick_store> [workdir] [codec]
set -euo pipefail

SYNTH="$1"
STORE="$2"
WORKDIR="${3:-${TMPDIR:-/tmp}}"
CODEC="${4:-raw}"
mkdir -p "$WORKDIR"
W="$WORKDIR/tick_store_smoke_$CODEC"
FEED="$W.itch"
TICKS="$W.ticks"

fail() { echo "error: $*" >&2; exit 1; }

# The exit status of a command that is expected to fail.
status_of() { set +e; "$@" >/dev/null 2>&1; local s=$?; set -e; echo "$s"; }

"$SYNTH" --seed 11 --messages 300000 --symbols 40 --live 5000 "$FEED" >/dev/null

# 1. Record, and check the store against a fresh replay of the feed.
"$STORE" record --codec "$CODEC" --block 256 "$FEED" "$TICKS"
"$STORE" verify --against "$FEED" "$TICKS" | tee "$W.verify.txt"
grep -q "RESULT: the store holds exactly what the book publishes" "$W.verify.txt" \
    || fail "the store does not match the feed"
"$STORE" info "$TICKS" | tee "$W.info.txt"
grep -Eq "state +complete" "$W.info.txt" || fail "a finished store did not open as complete"

# 2. A range of time is exactly the rows of the full dump that fall inside it.
"$STORE" query --nanos "$TICKS" > "$W.all.csv" 2>/dev/null
total=$(($(wc -l < "$W.all.csv") - 1))
(( total > 1000 )) || fail "only $total ticks: the feed is too quiet to test with"
from="$(awk -F, -v n="$total" 'NR == int(n / 3) + 2 { print $1 }' "$W.all.csv")"
to="$(awk -F, -v n="$total" 'NR == int(2 * n / 3) + 2 { print $1 }' "$W.all.csv")"
"$STORE" query --nanos --from "$from" --to "$to" "$TICKS" > "$W.range.csv" 2> "$W.range.err"
awk -F, -v from="$from" -v to="$to" 'NR == 1 || ($1 + 0 >= from + 0 && $1 + 0 < to + 0)' \
    "$W.all.csv" > "$W.range.expected.csv"
cmp "$W.range.csv" "$W.range.expected.csv" || fail "a time range gave the wrong ticks"
grep -Eq ", [1-9][0-9]* ruled out" "$W.range.err" \
    || fail "the query read every block: the index ruled none out"
counted="$("$STORE" query --count --from "$from" --to "$to" "$TICKS" 2>/dev/null)"
[[ "$counted" == "$(($(wc -l < "$W.range.csv") - 1))" ]] || fail "--count disagrees with the rows"

# One security, by the name in the store's directory.
symbol="$(awk -F, 'NR == 2 { print $2 }' "$W.all.csv")"
[[ -n "$symbol" ]] || fail "the store has no names"
"$STORE" query --nanos --symbol "$symbol" "$TICKS" > "$W.symbol.csv" 2>/dev/null
awk -F, -v s="$symbol" 'NR == 1 || $2 == s' "$W.all.csv" > "$W.symbol.expected.csv"
cmp "$W.symbol.csv" "$W.symbol.expected.csv" || fail "a query by symbol gave the wrong ticks"

# The profile of the store is the profile of the feed it was recorded from.
"$STORE" profile "$FEED" > "$W.profile.feed.txt"
"$STORE" profile "$TICKS" > "$W.profile.store.txt"
grep -q "which fields differ" "$W.profile.feed.txt" || fail "the profile is missing a section"
cmp "$W.profile.feed.txt" "$W.profile.store.txt" || fail "the two profiles differ"

# 3. A recorder that was killed: the file ends in the middle of a block. What
#    is left is the beginning of the same ticks.
size=$(wc -c < "$TICKS")
head -c $((size * 2 / 3)) "$TICKS" > "$W.cut.ticks"
"$STORE" info "$W.cut.ticks" > "$W.cut.info.txt"
grep -q "recovered by walking the blocks" "$W.cut.info.txt" \
    || fail "a store cut short was not reported as recovered"
"$STORE" verify "$W.cut.ticks" >/dev/null || fail "a store cut short did not verify"
"$STORE" query --nanos "$W.cut.ticks" 2>/dev/null | cut -d, -f1,3- > "$W.cut.csv"
kept=$(($(wc -l < "$W.cut.csv") - 1))
(( kept > 0 && kept < total )) || fail "a store cut to two thirds kept $kept of $total ticks"
head -n $((kept + 1)) "$W.all.csv" | cut -d, -f1,3- | cmp - "$W.cut.csv" \
    || fail "a store cut short does not hold the beginning of the full one"
[[ "$(status_of "$STORE" verify --against "$FEED" "$W.cut.ticks")" == 3 ]] \
    || fail "a store cut short was said to match the whole feed"

# 4. Damage after the fact: one byte of the ticks of block 3 changed.
offset="$("$STORE" info --blocks "$TICKS" | awk '$1 == "3" && NF == 6 { print $2 }')"
[[ -n "$offset" ]] || fail "could not find block 3 in the index"
at=$((offset + 32 + 10))
cp "$TICKS" "$W.bad.ticks"
old="$(od -An -tu1 -j "$at" -N1 "$TICKS" | tr -d ' ')"
printf "\\$(printf '%03o' $(((old + 1) % 256)))" \
    | dd of="$W.bad.ticks" bs=1 seek="$at" conv=notrunc status=none
[[ "$(status_of "$STORE" verify "$W.bad.ticks")" == 2 ]] \
    || fail "a damaged block was not reported"
"$STORE" query --count --to "$from" "$W.bad.ticks" >/dev/null 2>&1 \
    || true  # may or may not reach the damaged block; it must only not crash

# 5. What is not a store is refused.
[[ "$(status_of "$STORE" info "$FEED")" == 2 ]] || fail "an ITCH file was taken for a tick store"

echo "tick store smoke ok ($CODEC: $total ticks, $kept left after the cut)"
