#!/usr/bin/env python3
"""Turn two replay_bench JSON files into the before/after table for a log entry.

    scripts/compare_runs.py results/<before>/result.json results/<after>/result.json

Prints a Markdown table (median, with the range across runs) and the change in
each figure, plus the facts that decide whether the comparison is fair: same
file, same machine, same flags, and whether each book matched the reference.

It reports; it does not judge. A change smaller than the run-to-run range of
either side is noise, and the script says so, but the verdict is yours to
write.
"""

import json
import statistics
import sys

METRICS = [
    ("msgs/s", "msgs_per_s", "{:,.0f}", True),
    ("mean ns", "mean_ns", "{:.1f}", False),
    ("p50 ns", "p50_ns", "{:.1f}", False),
    ("p99 ns", "p99_ns", "{:.1f}", False),
    ("p99.9 ns", "p999_ns", "{:.1f}", False),
    ("max ns", "max_ns", "{:,.0f}", False),
]
COUNTERS = [
    ("cycles / msg", "cycles"),
    ("instructions / msg", "instructions"),
    ("cache misses / msg", "cache-misses"),
    ("branch misses / msg", "branch-misses"),
]
MUST_MATCH = ["file", "messages", "cpu_model", "compiler", "build_flags", "clock", "pinned_cpu"]


def summary(values):
    return statistics.median(values), min(values), max(values)


def cell(fmt, values):
    med, low, high = summary(values)
    return "%s (%s to %s)" % (fmt.format(med), fmt.format(low), fmt.format(high))


def spread(values):
    med, low, high = summary(values)
    return 0.0 if med == 0 else 100.0 * (high - low) / med


def per_message(doc, key):
    runs = [r for r in doc["runs"] if key in r]
    if len(runs) != len(doc["runs"]) or not runs:
        return None
    return [r[key] / doc["messages"] for r in runs]


def describe(doc):
    bits = [doc.get("workload", "?")]
    if doc.get("reserve"):
        bits.append("reserve %s" % format(doc["reserve"], ","))
    if doc.get("prefetch"):
        bits.append("prefetch")
    if doc.get("symbols"):
        bits.append("symbols %s" % doc["symbols"])
    if doc.get("label"):
        bits.append('label "%s"' % doc["label"])
    return ", ".join(bits)


def correctness(doc):
    if "book_invariants_clean" not in doc:
        return "no book in this workload"
    parts = ["invariants clean" if doc["book_invariants_clean"] else "INVARIANTS VIOLATED"]
    if doc.get("compared_with_reference"):
        parts.append("matches reference" if doc["matches_reference"] else "DIFFERS FROM REFERENCE")
    parts.append("hash %s" % doc.get("bbo_hash", "?"))
    return ", ".join(parts)


def main(argv):
    if len(argv) != 3 or argv[1] in ("-h", "--help"):
        sys.stderr.write(__doc__)
        return 1
    with open(argv[1]) as f:
        before = json.load(f)
    with open(argv[2]) as f:
        after = json.load(f)

    print("Before: %s" % describe(before))
    print("After:  %s" % describe(after))
    print()
    print("| | Before | After | Change |")
    print("|---|---|---|---|")
    noisy = []
    for name, key, fmt, higher_is_better in METRICS:
        b = [r[key] for r in before["runs"]]
        a = [r[key] for r in after["runs"]]
        b_med, a_med = statistics.median(b), statistics.median(a)
        change = 0.0 if b_med == 0 else 100.0 * (a_med - b_med) / b_med
        print("| %s | %s | %s | %+.1f%% |" % (name, cell(fmt, b), cell(fmt, a), change))
        if key in ("msgs_per_s", "p50_ns", "p99_ns") and abs(change) <= max(spread(b), spread(a)):
            noisy.append(name)
    for name, key in COUNTERS:
        b, a = per_message(before, key), per_message(after, key)
        if b is None or a is None:
            continue
        b_med, a_med = statistics.median(b), statistics.median(a)
        change = 0.0 if b_med == 0 else 100.0 * (a_med - b_med) / b_med
        print("| %s | %s | %s | %+.1f%% |" % (name, cell("{:.3f}", b), cell("{:.3f}", a), change))
    b_rate = statistics.median([r["msgs_per_s"] for r in before["runs"]])
    a_rate = statistics.median([r["msgs_per_s"] for r in after["runs"]])
    if b_rate > 0:
        print()
        print("Throughput ratio (after / before): %.2fx" % (a_rate / b_rate))

    print()
    print("Timer cost p50: before %.1f ns, after %.1f ns (included in every percentile)."
          % (before.get("timer_floor_p50_ns", 0), after.get("timer_floor_p50_ns", 0)))
    print("Runs: before %d, after %d. Throughput spread: before %.1f%%, after %.1f%%."
          % (len(before["runs"]), len(after["runs"]),
             spread([r["msgs_per_s"] for r in before["runs"]]),
             spread([r["msgs_per_s"] for r in after["runs"]])))
    print("Correctness before: %s" % correctness(before))
    print("Correctness after:  %s" % correctness(after))

    problems = []
    for key in MUST_MATCH:
        if before.get(key) != after.get(key):
            problems.append("%s differs: %r versus %r" % (key, before.get(key), after.get(key)))
    for doc, which in ((before, "before"), (after, "after")):
        if doc.get("book_invariants_clean") is False:
            problems.append("%s: the book violated its invariants" % which)
        if doc.get("compared_with_reference") and not doc.get("matches_reference"):
            problems.append("%s: the book differs from the reference, so its timings are void" % which)
        if len(doc["runs"]) < 5:
            problems.append("%s: fewer than 5 runs" % which)
        if doc.get("build_type") != "Release":
            problems.append("%s: not a Release build" % which)
    if noisy:
        problems.append("the change in %s is within the run-to-run range: treat it as noise"
                        % ", ".join(noisy))
    if problems:
        print()
        print("Check before trusting this comparison:")
        for p in problems:
            print("- %s" % p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
