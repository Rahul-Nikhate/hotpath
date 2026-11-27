#!/usr/bin/env python3
"""Plot the CSVs written by `hp_bench ... --csv DIR` and `run_demo.sh`.

    python3 scripts/plot.py results/<host>        # writes DIR/*.png
Needs matplotlib (pip install matplotlib).
"""
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def read(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def bar(path, rows, label_key, value_key, title, ylabel):
    labels = [r[label_key] for r in rows]
    values = [float(r[value_key]) for r in rows]
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.barh(labels, values, color="#4c72b0")
    ax.invert_yaxis()
    ax.set_xlabel(ylabel)
    ax.set_title(title)
    for i, v in enumerate(values):
        ax.text(v, i, f" {v:.1f}", va="center")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def percentiles(path, rows, name_key, title):
    cols = ["p50_ns", "p90_ns", "p99_ns", "p999_ns"]
    fig, ax = plt.subplots(figsize=(8, 4))
    for r in rows:
        ax.plot(["p50", "p90", "p99", "p99.9"], [float(r[c]) for c in cols], marker="o", label=r[name_key])
    ax.set_yscale("log")
    ax.set_ylabel("nanoseconds (log)")
    ax.set_title(title)
    ax.legend()
    ax.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "results"
    p = lambda n: os.path.join(d, n)  # noqa: E731
    if os.path.exists(p("queue_throughput.csv")):
        bar(p("queue_throughput.png"), read(p("queue_throughput.csv")), "variant", "mops",
            "Experiment 1b: SPSC queue, one change at a time", "million items / s")
    if os.path.exists(p("queue_hop.csv")):
        percentiles(p("queue_hop.png"), read(p("queue_hop.csv")), "metric", "Round trip between cores")
    if os.path.exists(p("fastpath.csv")):
        rows = read(p("fastpath.csv"))
        percentiles(p("fastpath.png"), rows, "scenario", "Decision latency (message -> order bytes)")
    if os.path.exists(p("pages.csv")):
        bar(p("pages.png"), read(p("pages.csv")), "variant", "first_touch_ns_per_record",
            "Experiment 2: first touch of a fresh region", "ns per 64 B record")
    if os.path.exists(p("dispatch.csv")):
        bar(p("dispatch.png"), read(p("dispatch.csv")), "variant", "ns_per_call",
            "Stretch 6: strategy dispatch", "ns per call")
    for sub in ("e2e-unpinned", "e2e-isolated", "demo"):
        lat = os.path.join(d, sub, "latency.csv")
        if os.path.exists(lat):
            rows = [r for r in read(lat) if r["metric"].startswith(("decision", "to-wire"))]
            percentiles(os.path.join(d, sub, "latency.png"), rows, "metric", f"End to end: {sub}")
    print("plots written to", d)


if __name__ == "__main__":
    main()
