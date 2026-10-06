#!/usr/bin/env bash
# Kills a journaling matching engine over and over, and checks that nothing a
# crash can do loses or changes a request that reached the journal.
#
#   scripts/journal_crash_test.sh <engine_journal> [workdir] [seed] [engine]
#
# The standard is simple to state. One run is made that is never killed. Every
# other run is killed again and again, restarted each time with the same
# options, until it finishes. Its journal must then be THE SAME FILE, byte for
# byte, as the one that was never killed, and the engine must hold the same
# state. Anything recovery got wrong along the way (a record lost, applied
# twice, or applied to the wrong state) changes what the engine does next, and
# so changes every byte written after it.
#
# What is done:
#
#   1. The run that is never killed.
#   2. Journal only. Each restart is killed in the middle of a write, at a
#      random write and after a random part of it (--kill-at-flush and
#      --kill-keep: the process sends itself SIGKILL). Recovery has to find
#      the torn record, cut it off and carry on.
#   3. The same with snapshots and with every write forced onto the disk, so
#      that recovery loads a snapshot and replays only the records after it.
#   4. A journal left with a torn tail is reported as that by `check`, which
#      must not change the file.
#   5. A journal damaged in the middle is refused by `check` and by `run`,
#      and is left exactly as it was: good records follow the damage, and
#      cutting the file there would throw them away.
#
# The kills are chosen by $RANDOM from [seed], which is printed, so a failure
# can be repeated.
#
# Exit status: 0 all of that held, 1 something did not, 4 the engine named is
# not written yet.
set -uo pipefail

BIN="$1"
WORKDIR="${2:-${TMPDIR:-/tmp}/journal_crash_test}"
SEED="${3:-20261006}"
ENGINE="${4:-reference}"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"
RANDOM=$SEED

FLOW=(--seed 4242 --commands 20000 --symbols 4 --live 300 --engine "$ENGINE")
MAX_ROUNDS=300

fail() {
    echo "error: $*" >&2
    exit 1
}

digest_of() {
    sed -n 's/^state digest: //p' "$1"
}

# Runs `engine_journal run` with the given options until it ends by itself,
# killing it each time before it can. Sets KILLS, TORN and LOADED to what
# happened on the way.
run_until_done() {
    local name="$1"
    local range="$2"  # the kill comes at a write chosen from 1 to this
    shift 2
    KILLS=0
    TORN=0
    LOADED=0
    local round status
    for ((round = 1; round <= MAX_ROUNDS; ++round)); do
        local at=$((RANDOM % range + 1))
        local keep=$((RANDOM % 101))
        # In the background and collected with wait, so that the shell does
        # not announce each kill on the terminal.
        "$BIN" run "${FLOW[@]}" "$@" --kill-at-flush "$at" --kill-keep "$keep" \
            >"$WORKDIR/$name.out" 2>"$WORKDIR/$name.err" &
        wait $! 2>/dev/null
        status=$?
        if grep -q "torn tail" "$WORKDIR/$name.out"; then TORN=$((TORN + 1)); fi
        if grep -q "snapshot .*loaded" "$WORKDIR/$name.out"; then LOADED=$((LOADED + 1)); fi
        case $status in
            0) return 0 ;;
            137) KILLS=$((KILLS + 1)) ;;
            4)
                cat "$WORKDIR/$name.err" >&2
                exit 4
                ;;
            *)
                cat "$WORKDIR/$name.out" "$WORKDIR/$name.err" >&2
                fail "$name: round $round (kill at write $at, keep $keep%) ended with status $status"
                ;;
        esac
    done
    fail "$name: still not finished after $MAX_ROUNDS rounds"
}

echo "journal crash test: engine $ENGINE, seed $SEED"

# 1. Never killed.
"$BIN" run "${FLOW[@]}" --journal "$WORKDIR/ref.j" >"$WORKDIR/ref.out" 2>"$WORKDIR/ref.err"
status=$?
if [ $status -eq 4 ]; then
    cat "$WORKDIR/ref.err" >&2
    exit 4
fi
[ $status -eq 0 ] || fail "the run that is never killed failed (status $status)"
REF=$(digest_of "$WORKDIR/ref.out")
[ -n "$REF" ] || fail "the reference run printed no digest"
echo "  1. never killed:        digest $REF, $(stat -c %s "$WORKDIR/ref.j") bytes"

# 2. Journal only, killed in the middle of writes.
run_until_done torn 400 --journal "$WORKDIR/torn.j" --batch 7
[ "$KILLS" -ge 5 ] || fail "only $KILLS kills: the run is too short to test anything"
[ "$TORN" -ge 1 ] || fail "no recovery ever met a torn tail"
cmp -s "$WORKDIR/torn.j" "$WORKDIR/ref.j" ||
    fail "after $KILLS kills the journal differs from the one that was never killed"
[ "$(digest_of "$WORKDIR/torn.out")" = "$REF" ] || fail "after $KILLS kills the engine state differs"
echo "  2. journal only:        $KILLS kills, $TORN torn tails cut, journal identical"

# 3. With snapshots. An engine that cannot be saved says so and is skipped.
"$BIN" run "${FLOW[@]}" --journal "$WORKDIR/probe.j" --snapshot "$WORKDIR/probe.s" \
    --commands 10 >"$WORKDIR/probe.out" 2>"$WORKDIR/probe.err"
if grep -q "cannot be saved to a snapshot" "$WORKDIR/probe.err"; then
    echo "  3. with snapshots:      skipped, the $ENGINE engine cannot be saved to one"
else
    run_until_done snap 40 --journal "$WORKDIR/snap.j" --snapshot "$WORKDIR/snap.s" \
        --snapshot-every 250 --batch 64 --sync flush
    [ "$KILLS" -ge 5 ] || fail "only $KILLS kills with snapshots"
    [ "$LOADED" -ge 1 ] || fail "no recovery ever loaded a snapshot"
    cmp -s "$WORKDIR/snap.j" "$WORKDIR/ref.j" ||
        fail "with snapshots, after $KILLS kills the journal differs"
    [ "$(digest_of "$WORKDIR/snap.out")" = "$REF" ] || fail "with snapshots the engine state differs"
    # The short way and the long way round arrive at the same place.
    "$BIN" check --engine "$ENGINE" --journal "$WORKDIR/snap.j" --snapshot "$WORKDIR/snap.s" \
        >"$WORKDIR/check_snap.out" || fail "check with the snapshot failed"
    "$BIN" check --engine "$ENGINE" --journal "$WORKDIR/snap.j" >"$WORKDIR/check_full.out" ||
        fail "check without the snapshot failed"
    grep -q "snapshot .*loaded" "$WORKDIR/check_snap.out" || fail "check did not load the snapshot"
    [ "$(digest_of "$WORKDIR/check_snap.out")" = "$REF" ] || fail "snapshot plus journal gives another state"
    [ "$(digest_of "$WORKDIR/check_full.out")" = "$REF" ] || fail "the whole journal gives another state"
    echo "  3. with snapshots:      $KILLS kills, $LOADED recoveries from a snapshot, journal identical"
fi

# 4. A torn tail is reported by check, which changes nothing.
cp "$WORKDIR/ref.j" "$WORKDIR/chopped.j"
truncate -s -5 "$WORKDIR/chopped.j"
BEFORE=$(stat -c %s "$WORKDIR/chopped.j")
"$BIN" check --engine "$ENGINE" --journal "$WORKDIR/chopped.j" >"$WORKDIR/chopped.out"
status=$?
[ $status -eq 2 ] || fail "check of a torn journal exited $status, not 2"
[ "$(stat -c %s "$WORKDIR/chopped.j")" = "$BEFORE" ] || fail "check changed the file"
# And a run puts it right: it cuts the tail and writes the lost request again.
"$BIN" run "${FLOW[@]}" --journal "$WORKDIR/chopped.j" >"$WORKDIR/chopped_run.out" ||
    fail "a run could not carry on a torn journal"
cmp -s "$WORKDIR/chopped.j" "$WORKDIR/ref.j" || fail "the carried-on journal differs"
echo "  4. torn tail:           reported, left alone by check, repaired by run"

# 5. Damage in the middle is refused, and the file is not touched.
cp "$WORKDIR/ref.j" "$WORKDIR/damaged.j"
MIDDLE=$(($(stat -c %s "$WORKDIR/ref.j") / 2))
printf '\377' | dd of="$WORKDIR/damaged.j" bs=1 seek="$MIDDLE" conv=notrunc status=none
cp "$WORKDIR/damaged.j" "$WORKDIR/damaged.copy"
cmp -s "$WORKDIR/damaged.j" "$WORKDIR/ref.j" && fail "the damage changed nothing; pick another byte"
"$BIN" check --engine "$ENGINE" --journal "$WORKDIR/damaged.j" >"$WORKDIR/damaged.out"
status=$?
[ $status -eq 3 ] || fail "check of a damaged journal exited $status, not 3"
"$BIN" run "${FLOW[@]}" --journal "$WORKDIR/damaged.j" >"$WORKDIR/damaged_run.out"
status=$?
[ $status -eq 3 ] || fail "run on a damaged journal exited $status, not 3"
cmp -s "$WORKDIR/damaged.j" "$WORKDIR/damaged.copy" || fail "a damaged journal was modified"
echo "  5. damage in the middle: refused by check and by run, file untouched"

echo "RESULT: ok"
