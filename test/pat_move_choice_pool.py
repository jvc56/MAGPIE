#!/usr/bin/env python3
"""Pools the POOL lines of sharded patmovechoice runs into one result.

Usage: pat_move_choice_pool.py shard_output_1.txt [shard_output_2.txt ...]

Each shard's POOL line carries its raw accumulators (positions,
disagreements, worlds, sum of per-position mean paired differences, sum
of their squares, sum of per-position across-world sample variances), so
the pooled mean, SE, 95% CI, effect per sampled decision and the
within/between variance decomposition are exactly what one unsharded run
over the same seeds would print.
"""
import math
import re
import sys

FIELD = re.compile(r'(\w+)=("[^"]*"|\S+)')


def main(paths):
    totals = {"positions": 0, "disagreements": 0, "sum_means": 0.0,
              "sum_means_sq": 0.0, "sum_within": 0.0}
    worlds = None
    labels = None
    shards = 0
    for path in paths:
        with open(path) as f:
            for line in f:
                if not line.startswith("POOL "):
                    continue
                fields = {k: v.strip('"') for k, v in FIELD.findall(line)}
                pair = (fields["candidate"], fields["baseline"])
                if labels is None:
                    labels = pair
                elif labels != pair:
                    sys.exit(f"{path}: mixes comparisons {labels} and {pair}")
                w = int(fields["worlds"])
                if worlds is None:
                    worlds = w
                elif worlds != w:
                    sys.exit(f"{path}: mixes world counts {worlds} and {w}")
                totals["positions"] += int(fields["positions"])
                totals["disagreements"] += int(fields["disagreements"])
                totals["sum_means"] += float(fields["sum_means"])
                totals["sum_means_sq"] += float(fields["sum_means_sq"])
                totals["sum_within"] += float(fields["sum_within"])
                shards += 1
    if shards == 0:
        sys.exit("no POOL lines found")
    n = totals["disagreements"]
    positions = totals["positions"]
    print(f"[{labels[0]}] vs [{labels[1]}]: {shards} shards pooled, "
          f"{positions} positions considered, {n} disagreements "
          f"({100.0 * n / positions if positions else 0.0:.2f}%)")
    if n < 2:
        return
    mean = totals["sum_means"] / n
    var_means = (totals["sum_means_sq"] / n - mean * mean) * (n / (n - 1))
    se = math.sqrt(max(var_means, 0.0) / n)
    within = totals["sum_within"] / n
    between = var_means - within / worlds
    print(f"  paired effect (candidate - baseline): mean {mean:.4f}, "
          f"SE {se:.4f}, 95% CI [{mean - 1.96 * se:.4f}, {mean + 1.96 * se:.4f}]")
    print(f"  effect per sampled decision (mean x disagreement rate): "
          f"{mean * n / positions:.4f}")
    if var_means > 0:
        print(f"  variance decomposition: within-position {within:.2f}, "
              f"between-position {between:.2f} (R = {worlds} worlds); shares "
              f"of Var(mean): between {100.0 * between / var_means:.1f}%, "
              f"within {100.0 * (within / worlds) / var_means:.1f}%")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
