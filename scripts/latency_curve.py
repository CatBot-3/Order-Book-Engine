#!/usr/bin/env python3
"""Turn load_gen's JSON into the latency-versus-throughput table and plot.

    scripts/latency_curve.py results/load.json                 # Markdown table
    scripts/latency_curve.py results/load.json --plot curve.png

Several files can be given, for example one per queue or per engine; each
becomes a series labelled with its file name.

The table is what goes in the README. The plot needs matplotlib and is skipped
with a message if it is not installed.

What the curve shows: at low rates the latency is the cost of one trip through
the software, and it barely moves as the rate rises. Then, at some rate, it
turns upwards sharply. That knee is the capacity of the slowest part of the
path. Past it requests arrive faster than they are served, a queue builds, and
latency is no longer a property of the software but of how long the run lasted.
Rows past the knee are worth showing, to show where it is, and not worth
quoting as latencies.
"""

import argparse
import json
import os
import sys


def load(path):
    with open(path) as f:
        data = json.load(f)
    if data.get("benchmark") != "load_gen":
        sys.exit(f"{path}: not a load_gen result")
    return data


def table(name, data):
    lines = [
        f"**{name}**: {data['connections']} connections, {data['seconds']} s measured "
        f"per rate after {data['warmup_seconds']} s warm-up, loopback",
        "",
        "| Target/s | Sent | Acknowledged | p50 µs | p90 µs | p99 µs | p99.9 µs | max µs | "
        "Generator lag max µs |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for r in data["rates"]:
        note = "" if r["unanswered"] == 0 else f" ({r['unanswered']:,} unanswered)"
        lines.append(
            f"| {r['target_rate']:,} | {r['sent']:,} | {r['acknowledged']:,}{note} | "
            f"{r['p50_us']:,.1f} | {r['p90_us']:,.1f} | {r['p99_us']:,.1f} | "
            f"{r['p999_us']:,.1f} | {r['max_us']:,.1f} | {r['worst_lag_us']:,.1f} |"
        )
    return "\n".join(lines)


def plot(series, path):
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not installed; skipping the plot", file=sys.stderr)
        return
    fig, ax = plt.subplots(figsize=(8, 5))
    for name, data in series:
        rates = [r["target_rate"] for r in data["rates"]]
        for key, style in (("p50_us", "-o"), ("p99_us", "--s"), ("p999_us", ":^")):
            ax.plot(rates, [r[key] for r in data["rates"]], style,
                    label=f"{name} {key[:-3].replace('p999', 'p99.9')}")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("requests per second")
    ax.set_ylabel("order-to-acknowledgement latency, microseconds")
    ax.set_title("Latency against throughput, loopback (software path only)")
    ax.grid(True, which="both", linewidth=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    print(f"wrote {path}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("results", nargs="+", help="JSON files written by load_gen --json")
    parser.add_argument("--plot", metavar="PNG", help="also draw the curve to this file")
    args = parser.parse_args()

    series = [(os.path.splitext(os.path.basename(p))[0], load(p)) for p in args.results]
    print("\n\n".join(table(name, data) for name, data in series))
    if args.plot:
        plot(series, args.plot)


if __name__ == "__main__":
    main()
