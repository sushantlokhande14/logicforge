#!/usr/bin/env python3
"""Compare two `lfc --json` files, e.g. from before and after a change.

Prints QoR, per-stage time and per-pass effect side by side and exits 1 if
QoR got worse, so it can gate a change:

    lfc design.sv --json old.json        # on main
    lfc design.sv --json new.json        # on your branch
    python3 tools/qor_diff.py old.json new.json

A drop in one pass's "removed" count usually points straight at the culprit.
"""
import json
import sys


def pct(a, b):
    return 0.0 if not a else 100.0 * (b - a) / a


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    old, new = (json.load(open(p)) for p in sys.argv[1:])
    worse = False

    print("QoR (after optimization)")
    for k in ("gates", "area", "depth", "flops"):
        a, b = old["after"][k], new["after"][k]
        flag = ""
        if b > a:
            flag, worse = "  <-- worse", True
        elif b < a:
            flag = "  better"
        print(f"  {k:<6} {a:>9} -> {b:<9} ({pct(a, b):+.1f}%){flag}")

    print("stages (ms)")
    for k in sorted(set(old["stages"]) | set(new["stages"])):
        a, b = old["stages"].get(k, 0), new["stages"].get(k, 0)
        print(f"  {k:<10} {a:>9.1f} -> {b:<9.1f} ({pct(a, b):+.1f}%)")

    print("passes (gates removed / ms)")
    for k in sorted(set(old["passes"]) | set(new["passes"])):
        a = old["passes"].get(k, {"removed": 0, "ms": 0})
        b = new["passes"].get(k, {"removed": 0, "ms": 0})
        flag = "  <-- removes less" if b["removed"] < a["removed"] else ""
        print(f"  {k:<10} {a['removed']:>8} -> {b['removed']:<8}  {a['ms']:>8.1f} -> {b['ms']:<8.1f}{flag}")

    sys.exit(1 if worse else 0)


if __name__ == "__main__":
    main()
