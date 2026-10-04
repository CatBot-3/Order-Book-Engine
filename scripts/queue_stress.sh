#!/usr/bin/env bash
# Success criterion 6, the stress half: push one billion numbered items
# through a queue from one thread to another and check that every one arrives
# once, in order, intact.
#
#   scripts/queue_stress.sh <queue_bench> [queue] [items]
#
#   scripts/queue_stress.sh build/release/bench/queue_bench ring
#   scripts/queue_stress.sh build/release/bench/queue_bench mutex 100000000
#
# The other half of the criterion is ThreadSanitizer:
#
#   cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
#
# A stress run alone proves less than it seems to. x86 will run a ring whose
# memory ordering is wrong for a trillion items without a single error,
# because the hardware orders more than the language requires. ThreadSanitizer
# checks the ordering the code actually asked for.
#
# Exit status: 0 every item arrived in order, 3 one did not, 4 the queue is
# not written yet.
set -euo pipefail

BENCH="$1"
QUEUE="${2:-ring}"
ITEMS="${3:-1000000000}"

exec "$BENCH" --queue "$QUEUE" --items "$ITEMS" --runs 1 --warmup 0 --label "ordered stress run"
