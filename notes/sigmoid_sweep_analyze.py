#!/usr/bin/env python3
"""Analyze sigmoid spread-utility sweep chunk logs.

Each arm ran as N chunks (distinct autoplay seeds). Every chunk log has an
"All Games" summary with wins/losses/ties and per-player mean scores.
P1 = control (uspread=0), P2 = treatment.

Stats per arm:
- Wins: exact two-sided binomial over decisive games (P2 wins vs P1 wins),
  pooled across chunks.
- Spread: batch-means t-test across chunks on mean spread-per-pair
  (2 * (mean_p2_score - mean_p1_score) since a pair is two games). Chunks
  are independent (distinct seeds), so a one-sample t-test on the chunk
  means against 0 is valid and preserves the pairing benefit within chunks.
"""
import re
import sys
from glob import glob

from scipy import stats as st

logdir = sys.argv[1]
arms = sys.argv[2:]  # arm names, e.g. u0.10 u0.25 u0.50 u1.00


def parse_log(path):
    text = open(path).read()
    all_games = text.split("All Games")[1].split("Divergent Games")[0]
    games = int(re.search(r"Games Played: (\d+)", all_games).group(1))
    wins = re.search(r"Wins:\s+(\d+) \([\d.]+%\)\s+(\d+) \([\d.]+%\)",
                     all_games)
    ties = re.search(r"Ties:\s+(\d+) \(", all_games)
    score = re.search(r"Score:\s+([\d.]+) ([\d.]+)\s+([\d.]+) ([\d.]+)",
                      all_games)
    return {
        "games": games,
        "p1_wins": int(wins.group(1)),
        "p2_wins": int(wins.group(2)),
        "ties": int(ties.group(1)),
        "p1_score": float(score.group(1)),
        "p2_score": float(score.group(3)),
    }


for arm in arms:
    logs = sorted(glob(f"{logdir}/{arm}_seed*.log"))
    if not logs:
        print(f"{arm}: no logs")
        continue
    chunks = [parse_log(p) for p in logs]
    games = sum(c["games"] for c in chunks)
    p1_wins = sum(c["p1_wins"] for c in chunks)
    p2_wins = sum(c["p2_wins"] for c in chunks)
    ties = sum(c["ties"] for c in chunks)
    decisive = p1_wins + p2_wins
    p_win = st.binomtest(p2_wins, decisive, 0.5).pvalue if decisive else 1.0
    # chunk-level mean spread per pair (pair = 2 games)
    spreads = [2 * (c["p2_score"] - c["p1_score"]) for c in chunks]
    if len(spreads) > 1:
        t_res = st.ttest_1samp(spreads, 0.0)
        p_spread = t_res.pvalue
    else:
        p_spread = float("nan")
    mean_spread = sum(s * c["games"] for s, c in zip(spreads, chunks)) / games
    p1_avg = sum(c["p1_score"] * c["games"] for c in chunks) / games
    p2_avg = sum(c["p2_score"] * c["games"] for c in chunks) / games
    print(f"{arm}: chunks={len(chunks)} games={games} pairs={games // 2} | "
          f"P2 wins={p2_wins} P1 wins={p1_wins} ties={ties} "
          f"P2 win%={100 * (p2_wins + ties / 2) / games:.2f} "
          f"p_win={p_win:.4f} | "
          f"spread/pair={mean_spread:+.2f} p_spread={p_spread:.4f} | "
          f"avg score P1={p1_avg:.1f} P2={p2_avg:.1f}")
