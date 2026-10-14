#!/usr/bin/env python3
"""Paired per-decision analysis of 1PEG choosers under three continuations.

Input: JSONL shards written by the peg1cont harness, one record per 1PEG board:
  {"id", "cgp", "last_tile", "unseen", "choices": {chooser: move_str},
   "eval": {move_str: {"static": spread, "twoply": spread, "exact": spread,
                       "exact_complete": bool, ...}}, ...}
Spreads are final (end-of-game) mover score minus opponent score, in points.

Output: JSON with, per continuation and chooser, the change per decision vs
static top-1 (wins and points, 95% bootstrap CI over boards), plus how much
each chooser's gain moves between continuations (paired), and how far the
static / two-ply continuations' outcomes sit from the exact solve.
"""
import argparse
import glob
import json
import sys

import numpy as np

CONTS = ["static", "twoply", "exact"]


def win(spread):
    return np.where(spread > 0, 1.0, np.where(spread < 0, 0.0, 0.5))


def boot_ci(values, rng, n_boot):
    values = np.asarray(values, dtype=np.float64)
    n = len(values)
    if n == 0:
        return [float("nan"), float("nan")]
    idx = rng.integers(0, n, size=(n_boot, n))
    means = values[idx].mean(axis=1)
    return [float(np.percentile(means, 2.5)), float(np.percentile(means, 97.5))]


def summarize(values, rng, n_boot):
    values = np.asarray(values, dtype=np.float64)
    return {
        "mean": float(values.mean()) if len(values) else float("nan"),
        "ci": boot_ci(values, rng, n_boot),
        "n": int(len(values)),
    }


def load(paths):
    records = []
    seen = set()
    for path in paths:
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                rec = json.loads(line)
                if rec["id"] in seen:
                    continue
                seen.add(rec["id"])
                records.append(rec)
    records.sort(key=lambda r: r["id"])
    return records


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shards", nargs="+")
    ap.add_argument("--boot", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=20261014)
    ap.add_argument("--require-exact-complete", action="store_true",
                    help="drop boards where any exact solve hit its limit")
    args = ap.parse_args()

    paths = []
    for pattern in args.shards:
        paths.extend(sorted(glob.glob(pattern)))
    records = load(paths)
    if args.require_exact_complete:
        records = [
            r for r in records
            if all(e.get("exact_complete", True) for e in r["eval"].values())
        ]
    rng = np.random.default_rng(args.seed)

    choosers = sorted({c for r in records for c in r["choices"]})
    out = {"boards": len(records), "choosers": {}, "continuation_fidelity": {}}

    def spreads(chooser, cont, subset):
        return np.array([
            r["eval"][r["choices"][chooser]][cont] for r in subset
        ], dtype=np.float64)

    for chooser in choosers:
        subset = [r for r in records if chooser in r["choices"]]
        entry = {"boards": len(subset)}
        entry["differs_from_static"] = float(np.mean([
            r["choices"][chooser] != r["choices"]["static"] for r in subset
        ])) if subset else float("nan")
        gains = {}
        for cont in CONTS:
            mine = spreads(chooser, cont, subset)
            base = spreads("static", cont, subset)
            dwin = win(mine) - win(base)
            dpts = mine - base
            gains[cont] = (dwin, dpts)
            entry[cont] = {
                "win_delta": summarize(dwin, rng, args.boot),
                "points_delta": summarize(dpts, rng, args.boot),
                "mover_win": float(win(mine).mean()),
                "mover_spread": float(mine.mean()),
                # Seat-swapped pair win score, as REPORT.md defines it.
                "pair_win_score": 0.5 + 0.5 * float(dwin.mean()),
            }
        # How much the measured gain moves when the continuation is fixed.
        for cont in ["static", "twoply"]:
            entry[f"gain_shift_{cont}_minus_exact"] = {
                "win": summarize(gains[cont][0] - gains["exact"][0], rng,
                                 args.boot),
                "points": summarize(gains[cont][1] - gains["exact"][1], rng,
                                    args.boot),
            }
        out["choosers"][chooser] = entry

    # Fidelity of each cheap continuation against the exact solve, over every
    # evaluated (board, move) pair.
    pairs = [(r, m) for r in records for m in r["eval"]]
    exact = np.array([r["eval"][m]["exact"] for r, m in pairs], dtype=float)
    for cont in ["static", "twoply"]:
        cheap = np.array([r["eval"][m][cont] for r, m in pairs], dtype=float)
        out["continuation_fidelity"][cont] = {
            "pairs": len(pairs),
            "mean_spread_minus_exact": float((cheap - exact).mean()),
            "mean_abs_spread_error": float(np.abs(cheap - exact).mean()),
            "win_outcome_agrees": float((win(cheap) == win(exact)).mean()),
            "mean_win_minus_exact": float((win(cheap) - win(exact)).mean()),
        }

    # Does the continuation change which of two moves looks better? Over
    # boards where a chooser differs from static: rank agreement with exact.
    rank = {}
    for chooser in choosers:
        if chooser == "static":
            continue
        subset = [r for r in records
                  if chooser in r["choices"]
                  and r["choices"][chooser] != r["choices"]["static"]]
        if not subset:
            continue
        ex = np.sign(spreads(chooser, "exact", subset) -
                     spreads("static", "exact", subset))
        row = {"boards": len(subset)}
        for cont in ["static", "twoply"]:
            ch = np.sign(spreads(chooser, cont, subset) -
                         spreads("static", cont, subset))
            row[f"{cont}_sign_agrees_with_exact"] = float((ch == ex).mean())
            row[f"{cont}_sign_flips_vs_exact"] = float((ch * ex < 0).mean())
        rank[chooser] = row
    out["pairwise_sign_vs_exact"] = rank

    json.dump(out, sys.stdout, indent=1)
    print()


if __name__ == "__main__":
    main()
