#!/usr/bin/env python3
"""Summarizes inferscore output (see test/infer_score_test.c).

Observations are split into train and test halves by a hash of their index.
For each evaluator the best hard margin and the best softmax temperature
are chosen on train by mean smoothed log P(true leave), then reported on
test, with paired differences against the baseline: the klv evaluator at
margin 5, which is the current static inference. Also reports raw zero
rates for hard margins, and mean rank and entropy on exactly enumerated
observations.

Usage: infer_score_summarize.py OUT.csv [OUT2.csv ...]
"""
import csv
import hashlib
import math
import sys
from collections import defaultdict

BASELINE = ("klv", "m5")


def load(paths):
    rows = defaultdict(dict)  # obs -> (evaluator, method) -> row
    for path in paths:
        with open(path) as handle:
            for row in csv.DictReader(handle):
                if row.get("evaluator") in (None, ""):
                    continue
                rows[int(row["obs"])][(row["evaluator"], row["method"])] = row
    return rows


def is_train(obs):
    # A real hash: observation ids are game * 100 + turn, so their low bits
    # track whose turn it was.
    return hashlib.md5(str(obs).encode()).digest()[0] % 2 == 0


def mean_se(values):
    n = len(values)
    if n == 0:
        return float("nan"), float("nan")
    mean = sum(values) / n
    if n < 2:
        return mean, float("nan")
    var = sum((v - mean) ** 2 for v in values) / (n - 1)
    return mean, math.sqrt(var / n)


def main():
    rows = load(sys.argv[1:])
    keys = sorted({k for per_obs in rows.values() for k in per_obs})
    evaluators = sorted({e for e, _ in keys})
    complete = [obs for obs, per_obs in rows.items()
                if all(k in per_obs for k in keys)]
    train = [o for o in complete if is_train(o)]
    test = [o for o in complete if not is_train(o)]
    print(f"observations: {len(complete)} complete "
          f"({len(train)} train, {len(test)} test)")

    def scores(obs_list, key):
        return [float(rows[o][key]["logp"]) for o in obs_list]

    chosen = []
    for evaluator in evaluators:
        for prefix in ("m", "t"):
            candidates = [k for k in keys if k[0] == evaluator
                          and k[1].startswith(prefix)]
            if not candidates:
                continue
            best = max(candidates,
                       key=lambda k: mean_se(scores(train, k))[0])
            chosen.append(best)
    if BASELINE in keys and BASELINE not in chosen:
        chosen.insert(0, BASELINE)

    print("\nsmoothed log P(true leave) on the test half (nats; higher is "
          "better), paired vs klv m5")
    print(f"{'evaluator':10s} {'method':7s} {'mean':>9s} {'se':>7s} "
          f"{'diff':>9s} {'se':>7s} {'zero%':>6s} {'rank':>7s} "
          f"{'entropy':>8s}")
    base = scores(test, BASELINE) if BASELINE in keys else None
    for key in chosen:
        values = scores(test, key)
        mean, se = mean_se(values)
        diff, dse = (float("nan"), float("nan"))
        if base is not None:
            diff, dse = mean_se([a - b for a, b in zip(values, base)])
        zeros = [int(rows[o][key]["zero"]) for o in test] or [0]
        exact = [o for o in test if rows[o][key]["mode"] == "exact"]
        ranks = [float(rows[o][key]["rank"]) for o in exact]
        ents = [float(rows[o][key]["entropy"]) for o in exact]
        print(f"{key[0]:10s} {key[1]:7s} {mean:9.4f} {se:7.4f} "
              f"{diff:9.4f} {dse:7.4f} {100 * sum(zeros) / len(zeros):6.2f} "
              f"{mean_se(ranks)[0]:7.2f} {mean_se(ents)[0]:8.3f}")
    prior = [float(rows[o][BASELINE]["log_prior"]) for o in test] \
        if BASELINE in keys else []
    if prior:
        print(f"\nprior alone (no model): {mean_se(prior)[0]:.4f} nats")
    print("\nevery method, test half:")
    for key in keys:
        mean, se = mean_se(scores(test, key))
        print(f"  {key[0]:6s} {key[1]:5s} {mean:9.4f} ± {se:.4f}")


if __name__ == "__main__":
    main()
