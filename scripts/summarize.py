#!/usr/bin/env python3
"""Render per-test comparison tables from mkbench raw CSVs.

Usage: summarize.py <results-dir>
"""
import csv
import os
import statistics
import sys
from collections import defaultdict

TIER_ORDER = ["T0", "T1", "T2", "T3",
              "local", "remote", "interleave",
              "1t", "sock0", "split"]
RATIO_PAIRS = [("T2", "T3", "T3/T2"),
               ("local", "remote", "rem/loc"),
               ("sock0", "split", "spl/sck")]
TEST_ORDER = ["memlat", "membw", "pingpong", "atomics", "lock", "ipc", "wakeup"]


def human_size(n):
    for shift, suffix in ((30, "g"), (20, "m"), (10, "k")):
        if n >= (1 << shift) and n % (1 << shift) == 0:
            return f"{n >> shift}{suffix}"
    return str(n)


def fmt(v):
    if v >= 1000:
        return f"{v:,.0f}"
    if v >= 10:
        return f"{v:.1f}"
    return f"{v:.2f}"


def main():
    res = sys.argv[1] if len(sys.argv) > 1 else "."
    raw = os.path.join(res, "raw")
    # (test, variant, metric, unit, size) -> tier -> [values]
    data = defaultdict(lambda: defaultdict(list))
    for fn in sorted(os.listdir(raw)):
        if not fn.endswith(".csv"):
            continue
        with open(os.path.join(raw, fn), newline="") as f:
            for row in csv.DictReader(f):
                key = (row["test"], row["variant"], row["metric"],
                       row["unit"], int(row["size"]))
                data[key][row["tier"]].append(float(row["value"]))

    tests = defaultdict(list)
    for key in data:
        tests[key[0]].append(key)

    for test in [t for t in TEST_ORDER if t in tests] + \
                sorted(set(tests) - set(TEST_ORDER)):
        keys = sorted(tests[test])
        tiers = []
        for key in keys:
            for t in data[key]:
                if t not in tiers:
                    tiers.append(t)
        tiers = [t for t in TIER_ORDER if t in tiers] + \
                [t for t in tiers if t not in TIER_ORDER]
        ratio_name = ""
        for base, cross, name in RATIO_PAIRS:
            if base in tiers and cross in tiers:
                ratio_base, ratio_cross, ratio_name = base, cross, name
                break

        header = ["variant", "metric", "size"] + tiers
        if ratio_name:
            header.append(ratio_name)
        rows = []
        for key in keys:
            _, variant, metric, unit, size = key
            med = {t: statistics.median(v) for t, v in data[key].items()}
            row = [variant, f"{metric}({unit})",
                   human_size(size) if size else "-"]
            row += [fmt(med[t]) if t in med else "-" for t in tiers]
            if ratio_name:
                if ratio_base in med and ratio_cross in med and med[ratio_base]:
                    row.append(f"{med[ratio_cross] / med[ratio_base]:.2f}x")
                else:
                    row.append("-")
            rows.append(row)

        widths = [max(len(r[i]) for r in [header] + rows)
                  for i in range(len(header))]
        print(f"== {test}")
        for r in [header] + rows:
            print("  " + "  ".join(c.ljust(w) for c, w in zip(r, widths)))
        print()


if __name__ == "__main__":
    main()
