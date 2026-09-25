#!/usr/bin/env python3
"""Thread-scaling benchmark for lfc.

Runs lfc on one design at several thread counts, several times each, and
prints the median wall time plus speedup over -j1. Every run is checked to
produce the same netlist, so a speedup never comes from doing less work.

    python3 tools/bench.py --lfc build/release/lfc --design bench.sv
"""
import argparse
import hashlib
import json
import os
import statistics
import subprocess
import tempfile


def run(lfc, design, threads, tmp):
    js = os.path.join(tmp, f"j{threads}.json")
    net = os.path.join(tmp, f"j{threads}.v")
    subprocess.run([lfc, design, "-j", str(threads), "--json", js, "-o", net], check=True,
                   stdout=subprocess.DEVNULL)
    with open(js) as f:
        stats = json.load(f)
    with open(net, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    return stats, digest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lfc", default="build/release/lfc")
    ap.add_argument("--design", required=True)
    ap.add_argument("--threads", default="1,2,4,8,12,16")
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--out", help="write results as JSON here")
    args = ap.parse_args()
    threads = [int(t) for t in args.threads.split(",")]

    rows, ref_digest, info = [], None, None
    with tempfile.TemporaryDirectory() as tmp:
        run(args.lfc, args.design, threads[0], tmp)  # warm the page cache
        for t in threads:
            totals, opts = [], []
            for _ in range(args.reps):
                stats, digest = run(args.lfc, args.design, t, tmp)
                if ref_digest is None:
                    ref_digest, info = digest, stats
                elif digest != ref_digest:
                    raise SystemExit(f"netlist changed at -j{t}: parallel run is not deterministic")
                totals.append(stats["total_ms"])
                opts.append(stats["stages"]["optimize"])
            rows.append({"threads": t, "total_ms": statistics.median(totals),
                         "optimize_ms": statistics.median(opts)})

    base = rows[0]
    print(f"design: {args.design}")
    print(f"  {info['before']['gates']} gates elaborated -> {info['after']['gates']} optimized, "
          f"{info['regions']} regions, {args.reps} runs per point, medians")
    print(f"  {'threads':>7}  {'total ms':>9}  {'speedup':>7}  {'optimize ms':>11}  {'speedup':>7}")
    for r in rows:
        r["speedup"] = base["total_ms"] / r["total_ms"]
        r["optimize_speedup"] = base["optimize_ms"] / r["optimize_ms"]
        print(f"  {r['threads']:>7}  {r['total_ms']:>9.1f}  {r['speedup']:>6.2f}x  "
              f"{r['optimize_ms']:>11.1f}  {r['optimize_speedup']:>6.2f}x")
    if args.out:
        with open(args.out, "w") as f:
            json.dump({"design": args.design, "reps": args.reps, "before": info["before"],
                       "after": info["after"], "regions": info["regions"], "rows": rows}, f, indent=2)


if __name__ == "__main__":
    main()
