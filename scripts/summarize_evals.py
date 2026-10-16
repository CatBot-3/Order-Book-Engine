#!/usr/bin/env python3
"""Turn the results directory of scripts/real_data_evals.sh into SUMMARY.md.

    scripts/summarize_evals.py results/real-<time> > SUMMARY.md

It reads what the stages wrote (status.tsv, the JSON reports, a few text
files) and prints Markdown: the machine, the success criteria with pass or
fail and where the evidence is, then one table per benchmark. It reports; it
does not judge. A stage that did not run, or a file that is missing, shows as
such instead of stopping the summary.
"""

import json
import pathlib
import statistics
import sys


def load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def read_text(path, limit=None):
    try:
        text = pathlib.Path(path).read_text(errors="replace")
    except OSError:
        return None
    if limit is not None:
        text = "\n".join(text.splitlines()[:limit])
    return text


def statuses(root):
    """The last recorded status of every stage."""
    out = {}
    text = read_text(root / "status.tsv")
    if not text:
        return out
    for line in text.splitlines()[1:]:
        parts = line.split("\t")
        if len(parts) >= 3:
            out[parts[0]] = (int(parts[1]) if parts[1].lstrip("-").isdigit() else parts[1], parts[2])
    return out


def fmt_rate(v):
    return "" if v is None else f"{v:,.0f}"


def fmt_ns(v):
    return "" if v is None else f"{v:,.1f}"


def median_of(runs, key):
    values = [r[key] for r in runs or [] if key in r]
    return statistics.median(values) if values else None


def spread(d):
    lo, hi, med = d.get("min_msgs_per_s"), d.get("max_msgs_per_s"), d.get("median_msgs_per_s")
    if not lo or not hi or not med:
        return ""
    return f"{(hi - lo) / med * 100:.1f}%"


def stage_mark(st, stage):
    if stage not in st:
        return "not run"
    status, seconds = st[stage]
    return ("pass" if status == 0 else f"FAIL ({status})") + f", {seconds} s"


def section_env(root, out):
    out.append("## Machine\n")
    env = read_text(root / "env" / "env.txt")
    out.append("```\n" + (env.strip() if env else "env stage did not run") + "\n```\n")
    data = read_text(root / "data" / "data.txt")
    if data:
        out.append("```\n" + data.strip() + "\n```\n")


def section_criteria(root, st, out):
    out.append("## Success criteria (spec, section 3.3)\n")
    out.append("| # | Criterion | Stage | Result | Evidence |")
    out.append("|---|---|---|---|---|")
    rows = [
        ("1", "parser counts equal the independent counter", "counts", "counts/log.txt"),
        ("2", "full-day replay, every invariant counter zero", "replay", "replay/book_replay.txt"),
        ("3", "every optimized book equals the reference", "diff", "diff/log.txt"),
        ("4", "benchmark report: throughput and latency percentiles, 5+ runs", "bench",
         "bench/*.json"),
        ("5", "optimization log: 5+ experiments, 1+ rejected", "-",
         "written into docs/optimization-log.md from these results"),
        ("6a", "lock-free queue: one billion ordered items", "queues", "queues/stress_ring.txt"),
        ("6b", "lock-free queue: ThreadSanitizer silent", "tsan", "tsan/*.txt"),
        ("7", "the engine's feed rebuilds the engine's book", "engine", "engine/round_trip.txt"),
        ("8", "gateway latency percentiles at several rates", "gateway",
         "gateway/latency_curve.md"),
    ]
    stress = read_text(root / "queues" / "stress_ring.txt") or ""
    transfer = next((line.split("transfer", 1)[1].strip() for line in stress.splitlines()
                     if line.strip().startswith("transfer")), "")
    for num, what, stage, evidence in rows:
        result = "see log" if stage == "-" else stage_mark(st, stage)
        if num == "6a" and transfer:
            result += f" ({transfer.split(' items')[0]} items)"
        out.append(f"| {num} | {what} | {stage} | {result} | `{evidence}` |")
    out.append("")
    out.append("Every stage:\n")
    out.append("| Stage | Result |")
    out.append("|---|---|")
    for stage, (status, seconds) in st.items():
        if stage == "summary":
            continue  # written by this script; its row is from an earlier pass
        out.append(f"| {stage} | {'pass' if status == 0 else f'FAIL ({status})'}, {seconds} s |")
    out.append("")


def replay_rows(paths):
    rows = []
    for path in paths:
        d = load_json(path)
        if not d:
            continue
        runs = d.get("runs", [])
        rows.append(
            "| {label} | {rate} | {spread} | {mean} | {p50} | {p99} | {p999} | {mx} | {runs} | {ref} |".format(
                label=d.get("label", path.stem),
                rate=fmt_rate(d.get("median_msgs_per_s")),
                spread=spread(d),
                mean=fmt_ns(median_of(runs, "mean_ns")),
                p50=fmt_ns(median_of(runs, "p50_ns")),
                p99=fmt_ns(median_of(runs, "p99_ns")),
                p999=fmt_ns(median_of(runs, "p999_ns")),
                mx=fmt_ns(median_of(runs, "max_ns")),
                runs=len(runs),
                ref=("n/a" if not d.get("compared_with_reference")
                     else ("yes" if d.get("matches_reference") else "NO")),
            ))
    return rows


def section_replay(root, out):
    out.append("## Book replay: `replay_bench` on the day\n")
    out.append("Medians over the runs; per-message latency in nanoseconds; spread is "
               "(max - min) / median of the throughput.\n")
    header = ("| Row | msgs/s | spread | mean | p50 | p99 | p99.9 | max | runs | equals reference |\n"
              "|---|---|---|---|---|---|---|---|---|---|")
    order = ["parse", "reference", "reference-reserve", "pooled", "flat-store", "vector-levels",
             "flat-vector", "flat-vector-reserve", "flat-vector-reserve-prefetch"]
    paths = [root / "bench" / f"{name}.json" for name in order]
    paths += [root / "lto" / "lto.json", root / "pgo" / "pgo.json"]
    rows = replay_rows([p for p in paths if p.exists()])
    if rows:
        out.append(header)
        out.extend(rows)
    else:
        out.append("bench stage did not run.")
    out.append("")
    for cmp in sorted((root / "bench").glob("compare_*.md")):
        out.append(f"<details><summary>{cmp.stem}</summary>\n")
        out.append(read_text(cmp) or "")
        out.append("</details>\n")


def section_engine(root, out):
    out.append("## Matching engines: `engine_bench`\n")
    rows = []
    for name in ["reference", "pooled", "reference_instructions", "pooled_instructions"]:
        d = load_json(root / "engine" / f"engine_{name}.json")
        if not d:
            continue
        runs = d.get("runs", [])
        rows.append(f"| {name} | {fmt_rate(d.get('median_msgs_per_s'))} | {spread(d)} | "
                    f"{fmt_ns(median_of(runs, 'p50_ns'))} | {fmt_ns(median_of(runs, 'p99_ns'))} | "
                    f"{fmt_ns(median_of(runs, 'p999_ns'))} | {fmt_ns(median_of(runs, 'max_ns'))} | "
                    f"{'yes' if d.get('matches_reference') else 'NO'} |")
    if rows:
        out.append("| Engine | requests/s | spread | p50 ns | p99 ns | p99.9 ns | max ns | equals reference |")
        out.append("|---|---|---|---|---|---|---|---|")
        out.extend(rows)
    else:
        out.append("engine stage did not run.")
    out.append("")


def section_queues(root, out):
    out.append("## Queues: `queue_bench` and the stress run\n")
    rows = []
    for name in ["mutex", "mutex-spin", "ring", "ring_siblings"]:
        d = load_json(root / "queues" / f"queue_{name}.json")
        if not d:
            continue
        rows.append(f"| {name} | {d.get('producer_cpu')},{d.get('consumer_cpu')} | "
                    f"{fmt_rate(d.get('median_msgs_per_s'))} | {spread(d)} | "
                    f"{'yes' if d.get('in_order') else 'NO'} |")
    if rows:
        out.append("| Queue | CPUs | items/s | spread | in order |")
        out.append("|---|---|---|---|---|")
        out.extend(rows)
    stress = read_text(root / "queues" / "stress_ring.txt")
    if stress:
        keep = ("transfer", "threads", "order", "run ", "median", "WARNING", "error")
        lines = [line for line in stress.splitlines() if line.strip().startswith(keep)]
        out.append("\nThe ordered stress run:\n\n```\n" + "\n".join(lines) + "\n```")
    out.append("")
    out.append("## Pipeline: three threads against one\n")
    rows = []
    for name in ["mutex", "ring"]:
        d = load_json(root / "pipeline" / f"pipeline_{name}.json")
        if not d:
            continue
        rows.append(f"| {name} | {fmt_rate(d.get('single_median_msgs_per_s'))} | "
                    f"{fmt_rate(d.get('median_msgs_per_s'))} | {d.get('pipeline_over_single', '')} | "
                    f"{'yes' if d.get('matches_single_thread') else 'NO'} |")
    if rows:
        out.append("| Queue | one thread msgs/s | pipeline msgs/s | pipeline / one | same output |")
        out.append("|---|---|---|---|---|")
        out.extend(rows)
    else:
        out.append("pipeline stage did not run.")
    out.append("")


def section_gateway(root, out):
    out.append("## Gateway: order-to-acknowledgement latency\n")
    curve = read_text(root / "gateway" / "latency_curve.md")
    out.append(curve.strip() if curve else "gateway stage did not run.")
    pinning = read_text(root / "gateway" / "pinning.txt")
    if pinning:
        out.append("\nWhere each program ran:\n\n```\n" + pinning.strip() + "\n```")
    out.append("")
    out.append("## Transports: system calls per request\n")
    text = read_text(root / "transports" / "transports.txt")
    out.append("```\n" + text.strip() + "\n```" if text else "transports stage did not run.")
    out.append("")


def section_sim(root, out):
    out.append("## Market-making simulation (a simulation, not evidence of profit)\n")
    rows = []
    for name in ["fixed", "join", "as"]:
        d = load_json(root / "sim" / f"{name}.json")
        if not d:
            continue
        markouts = {m.get("seconds"): m.get("cents_per_share") for m in d.get("markouts", [])}
        mk = ", ".join(f"{s}s: {v:.3f}" for s, v in markouts.items() if v is not None)
        rows.append(f"| {name} | {d.get('fills')} | {d.get('bought')} / {d.get('sold')} | "
                    f"{d.get('profit')} | {d.get('earned_at_fills')} | {d.get('max_drawdown')} | {mk} |")
    if rows:
        out.append("| Strategy | fills | bought / sold | profit | earned at fills | max drawdown | markouts (cents/share) |")
        out.append("|---|---|---|---|---|---|---|")
        out.extend(rows)
        out.append("\nMoney figures are as mm_sim reports them; read the assumptions at the end of `sim/as.txt`.")
    else:
        out.append("sim stage did not run.")
    out.append("")


def section_ticks(root, out):
    out.append("## Tick store\n")
    sizes = read_text(root / "ticks" / "sizes.txt")
    if sizes:
        out.append("```\n" + sizes.strip() + "\n```")
    zstd = read_text(root / "ticks" / "zstd.txt")
    if zstd:
        out.append("```\n" + zstd.strip() + "\n```")
    bench = read_text(root / "ticks" / "tick_bench.txt")
    if bench:
        out.append("```\n" + "\n".join(bench.strip().splitlines()[-30:]) + "\n```")
    if not (sizes or bench):
        out.append("ticks stage did not run.")
    out.append("")
    out.append("## Journal\n")
    text = read_text(root / "journal" / "journal.txt")
    disk = read_text(root / "journal" / "disk.txt")
    if text:
        out.append("```\n" + "\n".join(text.strip().splitlines()[-30:]) + "\n```")
    if disk:
        out.append("```\n" + disk.strip() + "\n```")
    if not text:
        out.append("journal stage did not run.")
    out.append("")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = pathlib.Path(sys.argv[1])
    st = statuses(root)
    out = [f"# Real-data evaluation: {root.name}\n"]
    section_env(root, out)
    section_criteria(root, st, out)
    section_replay(root, out)
    section_engine(root, out)
    section_queues(root, out)
    section_gateway(root, out)
    section_sim(root, out)
    section_ticks(root, out)
    print("\n".join(out))
    # The summary's own row is from an earlier pass of this script.
    failed = [s for s, (status, _) in st.items() if status != 0 and s != "summary"]
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
