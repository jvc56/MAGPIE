#!/usr/bin/env python3
"""Analyze a games_study.sh run.

  games_analyze.py <out_dir>

Strength: player a's score per game (win 1, tie 0.5), averaged within each
pair so the pair is the unit, with a normal-theory interval; mean spread.
Speed: per-decision wall time for each player (total, move generation, the
checks), as distributions, plus how often checks ran and changed the static
choice, by bag and lead strata. Timings are wall clock on the machine
named in MANIFEST.txt with every worker busy; compare players within one run.
"""
import csv
import glob
import json
import math
import os
import statistics
import sys
from collections import defaultdict


def read(pattern):
    rows = []
    for path in sorted(glob.glob(pattern)):
        with open(path, newline='') as handle:
            rows.extend(csv.DictReader(handle))
    return rows


def quantiles(values):
    if not values:
        return None
    values = sorted(values)
    pick = lambda q: values[min(len(values) - 1, int(q * len(values)))]
    return {'n': len(values), 'mean': statistics.fmean(values),
            'p50': pick(0.5), 'p90': pick(0.9), 'p99': pick(0.99),
            'max': values[-1]}


def main():
    out_dir = sys.argv[1]
    games = read(os.path.join(out_dir, 'w*.games.csv'))
    pairs = defaultdict(list)
    spreads = []
    for row in games:
        pairs[int(row['pair'])].append(float(row['a_win']))
        spreads.append(int(row['a_spread']))
    pair_means = [statistics.fmean(v) for v in pairs.values() if len(v) == 2]
    n = len(pair_means)
    mean = statistics.fmean(pair_means) if n else float('nan')
    se = statistics.stdev(pair_means) / math.sqrt(n) if n > 1 else float('nan')
    moves = read(os.path.join(out_dir, 'w*.moves.csv'))
    timing = {}
    for player in ('a', 'b'):
        rows = [r for r in moves if r['player'] == player]
        checked = [r for r in rows if r['checked'] == '1']
        timing[player] = {
            'decisions': len(rows),
            'checked': len(checked),
            'changed': sum(1 for r in checked if r['changed'] == '1'),
            'total_ms': quantiles([float(r['total_ms']) for r in rows]),
            'movegen_ms': quantiles([float(r['movegen_ms']) for r in rows]),
            'check_ms_when_checked': quantiles(
                [float(r['check_ms']) for r in checked]),
        }
    strata = defaultdict(lambda: [0, 0])
    for row in moves:
        if row['checked'] != '1':
            continue
        bag, lead = int(row['bag']), int(row['lead'])
        phase = 'late' if bag < 30 else ('middle' if bag < 60 else 'early')
        lead_bin = ('behind' if lead < -40 else 'close' if lead < 40 else
                    'ahead40_80' if lead <= 80 else 'ahead80+')
        key = f'{phase}/{lead_bin}'
        strata[key][0] += 1
        strata[key][1] += row['changed'] == '1'
    print(json.dumps({
        'pairs': n, 'games': len(games),
        'a_score': mean, 'a_score_se': se,
        'a_score_95': [mean - 1.96 * se, mean + 1.96 * se] if n > 1 else None,
        'a_mean_spread': statistics.fmean(spreads) if spreads else None,
        'timing_ms': timing,
        'changed_by_stratum': {k: {'checked': v[0], 'changed': v[1]}
                               for k, v in sorted(strata.items())},
    }, indent=1))


if __name__ == '__main__':
    main()
