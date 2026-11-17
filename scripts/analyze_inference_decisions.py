#!/usr/bin/env python3
"""Summarise infdecide logs: per-arm decision-level gain over the control.

Each eligible position contributes one value per arm: 0 when the arm chose the
control's move, else the oracle difference (arm minus control) against the
opponent's actual rack. Per-game values sum one player's decisions (the mean
of the two seats), with the standard error taken across complete games.
"""
import math
import re
import statistics as st
import sys
from collections import defaultdict

NAMES = {'s': 'softmax inference', 'p': 'perfect information',
         'c': 'second control (other seed)', 't': 'top static move',
         'a': 'softmax inference T3', 'b': 'softmax inference T16',
         'h': 'hard-margin inference (5)', '1': 'one opponent tile known',
         '2': 'two opponent tiles known', '4': 'four opponent tiles known'}


def mean_ci(values):
    n = len(values)
    mean = st.mean(values)
    se = st.stdev(values) / math.sqrt(n) if n > 1 else float('nan')
    return mean, se


def main():
    rows = defaultdict(list)
    complete = set()
    seconds = defaultdict(list)
    for path in sys.argv[1:]:
        for line in open(path, errors='replace'):
            if line.startswith('INFDEC_ARM '):
                row = dict(re.findall(r'(\w+)=(\S+)', line))
                if 'same' not in row or (row['same'] == '0' and 'oracle_seconds' not in row):
                    continue
                rows[row['arm']].append(row)
            elif line.startswith('INFDEC_GAME '):
                row = dict(re.findall(r'(\w+)=(\S+)', line))
                complete.add((row['seed'], row['game']))
            elif line.startswith('INFDEC_POS '):
                row = dict(re.findall(r'(\w+)=(\S+)', line))
                for key, value in row.items():
                    if key.endswith('_seconds') and key[0] != 't':
                        seconds[key[0]].append(float(value))
    print(f'complete games: {len(complete)}')
    for arm, values in sorted(seconds.items()):
        label = 'control' if arm == '0' else NAMES.get(arm, arm)
        print(f'search seconds per move, {label}: mean {st.mean(values):.2f}, '
              f'median {st.median(values):.2f}, max {max(values):.2f} (n={len(values)})')
    for arm in 'spctabh124':
        if arm not in rows:
            continue
        data = rows[arm]
        n = len(data)
        differ = [r for r in data if r['same'] == '0']
        print(f'\n== {NAMES[arm]} vs control: {n} positions, '
              f'{len(differ)} differ ({100 * len(differ) / n:.1f}%)')
        for key, label, scale in (('dwin', 'playout oracle, win', 100),
                                  ('dspread', 'playout oracle, spread', 1),
                                  ('sim_dwin', 'sim oracle, win', 100),
                                  ('sim_dutility', 'sim oracle, utility', 100),
                                  ('sim_dequity', 'sim oracle, equity', 1)):
            per_decision = [float(r[key]) * scale if r['same'] == '0' else 0.0 for r in data]
            mean, se = mean_ci(per_decision)
            games = defaultdict(float)
            for r, value in zip(data, per_decision):
                if (r['seed'], r['game']) in complete:
                    games[(r['seed'], r['game'])] += value / 2
            for game in complete:
                games.setdefault(game, 0.0)
            text = (f'  {label}: per decision {mean:+.4f} (se {se:.4f}, '
                    f'z {mean / se if se else float("nan"):+.2f})')
            if len(games) > 1:
                gmean, gse = mean_ci(list(games.values()))
                text += (f'; per game {gmean:+.3f} [{gmean - 1.96 * gse:+.3f}, '
                         f'{gmean + 1.96 * gse:+.3f}]')
            if differ:
                dmean = st.mean(float(r[key]) * scale for r in differ)
                text += f'; when different {dmean:+.3f}'
            print(text)
        if differ:
            agree = sum((float(r['dwin']) > 0) == (float(r['sim_dwin']) > 0) for r in differ)
            print(f'  oracles agree in sign on {agree}/{len(differ)} differing positions')


if __name__ == '__main__':
    main()
