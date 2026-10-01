#!/usr/bin/env python3
"""Analyze a pools_study.sh run: paired reference WP of each pool's choice.

  pools_analyze.py <out_dir> <positions.csv> [--compare X:Y ...]

For each position and pool, the selection sim's chosen move is valued by the
independent reference sim (percentage points). A comparison X:Y reports the
paired mean of WP(choice of X) - WP(choice of Y) over every position,
including positions where both choose the same move, with a normal-theory
two-sided p value and a position bootstrap interval. Report every
prespecified comparison; do not pick among them after seeing the results.
"""
import argparse
import csv
import glob
import json
import math
import os
import random
import statistics
from collections import defaultdict


def read(pattern):
    rows = []
    for path in sorted(glob.glob(pattern)):
        with open(path, newline='') as handle:
            rows.extend(csv.DictReader(handle))
    return rows


def paired(diffs, seed=1, reps=4000):
    n = len(diffs)
    if n == 0:
        return {'n': 0}
    mean = statistics.fmean(diffs)
    se = statistics.stdev(diffs) / math.sqrt(n) if n > 1 else float('nan')
    z = mean / se if se and se > 0 else float('nan')
    p = math.erfc(abs(z) / math.sqrt(2)) if not math.isnan(z) else float('nan')
    rng = random.Random(seed)
    boots = sorted(statistics.fmean(rng.choices(diffs, k=n)) for _ in range(reps))
    changed = sum(1 for d in diffs if d != 0)
    return {'n': n, 'mean_pp': mean, 'se_pp': se, 'p_two_sided': p,
            'bootstrap95': [boots[int(0.025 * reps)], boots[int(0.975 * reps) - 1]],
            'positions_with_different_value': changed,
            'positive': sum(1 for d in diffs if d > 0),
            'negative': sum(1 for d in diffs if d < 0)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out_dir')
    parser.add_argument('positions')
    parser.add_argument('--compare', nargs='*', default=[
        'B:A', 'C:A', 'Bp:B', 'A:static_A', 'B:static_B', 'Bp:static_Bp',
        'C:static_C'])
    args = parser.parse_args()
    phase = {}
    with open(args.positions, newline='') as handle:
        for row in csv.DictReader(handle):
            phase[int(row['game'])] = (row['phase'], int(row['bag']),
                                       int(row['lead']))
    refs = defaultdict(dict)
    for row in read(os.path.join(args.out_dir, 'w*.refs.csv')):
        refs[int(row['game'])][row['move']] = 100.0 * float(row['ref_wp'])
    chosen = defaultdict(dict)
    counts = defaultdict(dict)
    for row in read(os.path.join(args.out_dir, 'w*.pools.csv')):
        chosen[int(row['game'])][row['pool']] = row['chosen']
        counts[int(row['game'])][row['pool']] = int(row['count'])
    report = {'positions': len(chosen), 'comparisons': {}, 'pool_sizes': {}}
    pools = sorted({pool for games in chosen.values() for pool in games})
    for pool in pools:
        sizes = [counts[g][pool] for g in counts if pool in counts[g]]
        report['pool_sizes'][pool] = {'mean': statistics.fmean(sizes),
                                      'min': min(sizes), 'max': max(sizes)}
    for spec in args.compare:
        left, right = spec.split(':')
        diffs, by_phase = [], defaultdict(list)
        for game in sorted(chosen):
            picks = chosen[game]
            if left not in picks or right not in picks:
                continue
            if game not in refs or len(refs[game]) == 0:
                diff = 0.0
            else:
                diff = refs[game][picks[left]] - refs[game][picks[right]]
            diffs.append(diff)
            by_phase[phase.get(game, ('?', 0, 0))[0]].append(diff)
        entry = paired(diffs)
        entry['by_phase'] = {name: paired(values)
                             for name, values in sorted(by_phase.items())}
        entry['different_choice'] = sum(
            1 for game in chosen if left in chosen[game] and
            right in chosen[game] and chosen[game][left] != chosen[game][right])
        report['comparisons'][spec] = entry
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
