"""How much better than a random leave are the leaves players keep? For
held-out decisions with a leave, the opponent's kept leave's value (MAGPIE's
KLV, from `convert klv2csv`) against leaves of the same size drawn at random
from the tiles unseen to the mover: the mean difference in equity points
and the actual leave's fractional rank among the random ones (mid-rank for
ties; 0.5 if players kept leaves like random draws).

    python leavequality.py --data "opp500k/*.bin.t*" --klv data/lexica/NWL23.csv
"""

import argparse

import numpy as np

from rackjoint import LETTERS, Data

NAMES = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def key(counts):
    return "".join(NAMES[l] * int(counts[l]) for l in range(LETTERS))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True)
    parser.add_argument("--klv", required=True)
    parser.add_argument("--val-mod", type=int, default=20)
    parser.add_argument("--max", type=int, default=20000)
    parser.add_argument("--samples", type=int, default=200)
    args = parser.parse_args()
    klv = {"": 0.0}
    for line in open(args.klv):
        leave, value = line.rsplit(",", 1)
        klv[leave] = float(value)
    data = Data(args.data, args.val_mod)
    refs = data.val[data.val[:, 3] == 1][:args.max]
    rng = np.random.default_rng(0)
    rows = []
    missing = 0
    for p, d, _, _ in refs:
        opp = data.opp[p][d]
        leave = opp["leave"][:LETTERS].astype(int)
        k = int(leave.sum())
        if k == 0:
            continue
        r = data.parts[p][data.starts[p][d] + data.played[p][d]]
        unseen = np.round(r["scalars"][27:54].astype(np.float64)
                          * (int(r["bag"]) + 7)).astype(int)
        pool = np.repeat(np.arange(LETTERS), unseen)
        order = np.argsort(rng.random((args.samples, len(pool))), axis=1)[:, :k]
        draws = pool[order]
        values = []
        for row in draws:
            counts = np.bincount(row, minlength=LETTERS)
            values.append(klv.get(key(counts), np.nan))
        values = np.array(values)
        actual = klv.get(key(leave), np.nan)
        if np.isnan(actual) or np.isnan(values).any():
            missing += 1
            continue
        rank = (values < actual).mean() + 0.5 * (values == actual).mean()
        rows.append((k, int(r["bag"]), actual, values.mean(), rank))
    rows = np.array(rows)
    print(f"{len(rows)} decisions ({missing} skipped for a missing leave value)")
    k, bag, actual, random, rank = rows.T
    print(f"mean leave value: kept {actual.mean():+.2f}, random same-size "
          f"{random.mean():+.2f}, difference {np.mean(actual - random):+.2f} "
          f"points (sd of the difference {np.std(actual - random):.2f}); "
          f"mean fractional rank {rank.mean():.3f}")
    for lo, hi, label in [(60, 999, "bag 60+"), (30, 60, "bag 30-59"),
                          (7, 30, "bag 7-29"), (0, 7, "bag 0-6")]:
        m = (bag >= lo) & (bag < hi)
        print(f"  {label:10s} n={int(m.sum()):6d} kept {actual[m].mean():+.2f} "
              f"random {random[m].mean():+.2f} diff "
              f"{np.mean(actual[m] - random[m]):+.2f} rank {rank[m].mean():.3f}")
    for size in range(1, 7):
        m = k == size
        if m.sum():
            print(f"  leave of {size} n={int(m.sum()):6d} kept "
                  f"{actual[m].mean():+.2f} random {random[m].mean():+.2f} diff "
                  f"{np.mean(actual[m] - random[m]):+.2f} rank {rank[m].mean():.3f}")


if __name__ == "__main__":
    main()
