#!/usr/bin/env python3
"""Markdown tables for the peg1cont notes, from the result shards.

  python3 tools/peg1cont/tables.py 'results/*/*.jsonl' > tables.md

Per-decision change vs static top-1 (wins in percentage points, points per
decision, 95% bootstrap CIs over boards) under each continuation, the shift
in each chooser's measured gain when the continuation changes, how far the
cheap continuations land from the exact solve, and the same comparison
restricted to the boards where MAGPIE's default PEG ran.
"""
import argparse
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze  # noqa: E402

CONTS = ["static", "twoply", "exact"]
ORDER = ["score", "oneply", "d2m", "d2w", "exm", "exw", "greedy64",
         "greedy_all", "pegfull"]
LABEL = {
    "score": "score only",
    "oneply": "one-ply reply rule",
    "d2m": "depth-2 teacher, margin first",
    "d2w": "depth-2 teacher, win first",
    "exm": "exact teacher (top 8), margin first",
    "exw": "exact teacher (top 8), win first",
    "greedy64": "PEG greedy seed, static top 64",
    "greedy_all": "PEG greedy seed, all moves",
    "pegfull": "MAGPIE PEG, default schedule",
}


def win(spread):
    return np.where(spread > 0, 1.0, np.where(spread < 0, 0.0, 0.5))


def boot(values, rng, n_boot):
    values = np.asarray(values, dtype=float)
    idx = rng.integers(0, len(values), size=(n_boot, len(values)))
    means = values[idx].mean(axis=1)
    return values.mean(), np.percentile(means, 2.5), np.percentile(means, 97.5)


def spreads(records, chooser, cont):
    return np.array([r["eval"][r["choices"][chooser]][cont] for r in records],
                    dtype=float)


def fmt_win(m, lo, hi):
    return f"{100 * m:+.2f}% [{100 * lo:+.1f}, {100 * hi:+.1f}]"


def fmt_pts(m, lo, hi):
    return f"{m:+.2f} [{lo:+.1f}, {hi:+.1f}]"


def gains_table(records, choosers, rng, n_boot):
    lines = ["| chooser | boards | differs | " +
             " | ".join(f"{c}: wins | {c}: points" for c in CONTS) + " |",
             "|---|---:|---:|" + "---:|---:|" * len(CONTS)]
    for chooser in choosers:
        subset = [r for r in records if chooser in r["choices"]]
        if not subset:
            continue
        differs = np.mean([r["choices"][chooser] != r["choices"]["static"]
                           for r in subset])
        cells = []
        for cont in CONTS:
            mine = spreads(subset, chooser, cont)
            base = spreads(subset, "static", cont)
            cells.append(fmt_win(*boot(win(mine) - win(base), rng, n_boot)))
            cells.append(fmt_pts(*boot(mine - base, rng, n_boot)))
        lines.append(f"| {LABEL[chooser]} | {len(subset)} | {differs:.2f} | " +
                     " | ".join(cells) + " |")
    return "\n".join(lines)


def shift_table(records, choosers, rng, n_boot):
    lines = ["| chooser | static minus exact: wins | points | "
             "two-ply minus exact: wins | points |",
             "|---|---:|---:|---:|---:|"]
    for chooser in choosers:
        subset = [r for r in records if chooser in r["choices"]]
        if not subset:
            continue
        gain = {}
        for cont in CONTS:
            mine = spreads(subset, chooser, cont)
            base = spreads(subset, "static", cont)
            gain[cont] = (win(mine) - win(base), mine - base)
        cells = []
        for cont in ["static", "twoply"]:
            cells.append(fmt_win(*boot(gain[cont][0] - gain["exact"][0], rng,
                                       n_boot)))
            cells.append(fmt_pts(*boot(gain[cont][1] - gain["exact"][1], rng,
                                       n_boot)))
        lines.append(f"| {LABEL[chooser]} | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def fidelity_table(records):
    pairs = [(r, m) for r in records for m in r["eval"]]
    exact = np.array([r["eval"][m]["exact"] for r, m in pairs], dtype=float)
    lines = ["| continuation | mover spread minus exact | mean abs error | "
             "W/T/L agrees with exact | mover wins minus exact |",
             "|---|---:|---:|---:|---:|"]
    for cont in ["static", "twoply"]:
        cheap = np.array([r["eval"][m][cont] for r, m in pairs], dtype=float)
        lines.append(
            f"| {cont} | {np.mean(cheap - exact):+.2f} | "
            f"{np.mean(np.abs(cheap - exact)):.2f} | "
            f"{100 * np.mean(win(cheap) == win(exact)):.1f}% | "
            f"{100 * np.mean(win(cheap) - win(exact)):+.2f}% |")
    return f"{len(pairs)} (board, move) pairs.\n\n" + "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shards", nargs="+")
    ap.add_argument("--boot", type=int, default=10000)
    ap.add_argument("--seed", type=int, default=20261014)
    args = ap.parse_args()
    paths = sorted(p for pattern in args.shards for p in glob.glob(pattern))
    records = analyze.load(paths)
    rng = np.random.default_rng(args.seed)
    choosers = [c for c in ORDER if any(c in r["choices"] for r in records)]
    proven = np.mean([e.get("exact_complete", True) for r in records
                      for e in r["eval"].values()])
    print(f"Boards: {len(records)}. Exact solves flagged unproven: "
          f"{100 * (1 - proven):.2f}% (runs before the flag count as proven)."
          "\n")
    print("### Change per 1PEG decision vs static top-1\n")
    print(gains_table(records, choosers, rng, args.boot))
    print("\n### How much each continuation moves the measured gain\n")
    print(shift_table(records, choosers, rng, args.boot))
    print("\n### Cheap continuations against the exact solve\n")
    print(fidelity_table(records))
    peg = [r for r in records if "pegfull" in r["choices"]]
    if peg:
        print(f"\n### Boards where MAGPIE's default PEG ran ({len(peg)})\n")
        print(gains_table(peg, choosers, rng, args.boot))
    passes = {c: sum(r["choices"].get(c) == "pass" for r in records)
              for c in choosers}
    print("\nPass picks: " + ", ".join(f"{c} {n}" for c, n in passes.items()
                                       if n))


if __name__ == "__main__":
    main()
