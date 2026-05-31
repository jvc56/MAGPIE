#!/usr/bin/env python3
"""Aggregate one or two golden_eval CSVs into strategy-quality summary stats.

Usage:
  aggregate_golden_eval.py <eval.csv>                 # single-strategy summary
  aggregate_golden_eval.py <eval_a.csv> <eval_b.csv>  # paired comparison

Both CSVs are produced by the inneragree `golden_eval` mode and must have been
evaluated against the same golden file (so the (game, turn, on_turn_player)
keys line up). Strategy A and B will be compared on paired-difference loss.
"""
import csv
import math
import statistics
import sys
from pathlib import Path
from collections import defaultdict


def load(path):
    rows = {}
    for r in csv.DictReader(open(path)):
        key = (int(r["game"]), int(r["turn"]), int(r["on_turn_player"]))
        rows[key] = r
    return rows


def summarize(rows, label="strategy"):
    losses = []
    agrees = 0
    not_found = 0
    for r in rows.values():
        if int(r["found_in_golden"]):
            losses.append(float(r["loss_vs_golden"]))
            if int(r["agree"]):
                agrees += 1
        else:
            not_found += 1
    n = len(rows)
    n_eval = len(losses)
    print(f"=== {label} ===  (n={n} positions, {n_eval} found in golden, "
          f"{not_found} not-found)")
    if not losses:
        print("  no usable rows"); return
    print(f"  agree rate (strategy pick == golden top): "
          f"{agrees}/{n_eval} = {100*agrees/n_eval:.1f}%")
    print(f"  loss vs golden: mean={statistics.mean(losses):+.4f}  "
          f"median={statistics.median(losses):+.4f}  "
          f"max={max(losses):+.4f}")
    if len(losses) > 1:
        sd = statistics.stdev(losses)
        se = sd / math.sqrt(len(losses))
        print(f"                  sd={sd:.4f}  se={se:.4f}  "
              f"z(vs 0)={statistics.mean(losses)/se:+.2f}")


def paired_compare(a, b, label_a="A", label_b="B"):
    keys = sorted(set(a.keys()) & set(b.keys()))
    diffs = []  # loss_a - loss_b ; positive = A worse than B
    agree_same = 0
    a_better = 0
    b_better = 0
    for k in keys:
        ra, rb = a[k], b[k]
        if not (int(ra["found_in_golden"]) and int(rb["found_in_golden"])):
            continue
        la = float(ra["loss_vs_golden"])
        lb = float(rb["loss_vs_golden"])
        diffs.append(la - lb)
        if la < lb: a_better += 1
        elif lb < la: b_better += 1
        else: agree_same += 1
    n = len(diffs)
    print(f"\n=== Paired comparison: {label_a} vs {label_b} ===")
    print(f"  {n} positions evaluable by both (intersection of keys, both "
          f"found in golden)")
    if not n: return
    mean_d = statistics.mean(diffs)
    sd_d = statistics.stdev(diffs) if n > 1 else 0
    se_d = sd_d / math.sqrt(n) if n > 1 else 0
    z = (mean_d / se_d) if se_d > 0 else 0
    print(f"  position-by-position loss diff (A - B):")
    print(f"    mean={mean_d:+.4f}  median={statistics.median(diffs):+.4f}  "
          f"sd={sd_d:.4f}  se={se_d:.4f}")
    print(f"    z(vs 0) = {z:+.2f}  (positive z means A worse, B better)")
    print(f"  win-vs-loss tally on the same positions:")
    print(f"    {label_a} lower loss: {a_better}    "
          f"{label_b} lower loss: {b_better}    tied: {agree_same}")


def main(args):
    if len(args) == 1:
        rows = load(args[0])
        summarize(rows, Path(args[0]).stem)
    elif len(args) == 2:
        ra = load(args[0]); rb = load(args[1])
        summarize(ra, Path(args[0]).stem)
        summarize(rb, Path(args[1]).stem)
        paired_compare(ra, rb, Path(args[0]).stem, Path(args[1]).stem)
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main(sys.argv[1:])
