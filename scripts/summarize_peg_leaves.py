#!/usr/bin/env python3
"""Summarise infbench pegBxT logs: leave prediction quality by policy and mode."""
import math
import re
import statistics as st
import sys
from collections import defaultdict

MODES = ['prior', 'hard0', 'hard5', 'hard10', 'soft5t2', 'soft10t5', 'soft20t5', 'softnocutofft8']
FIELDS = ['mixed_nll', 'gain_nats', 'klv_mse', 'vowel_mse', 'score_mse', 'tile_mse', 'duplicate_mse']


def main():
    rows = defaultdict(list)
    tiles = defaultdict(lambda: defaultdict(float))
    counts = defaultdict(int)
    leaves = defaultdict(list)
    for path in sys.argv[1:]:
        summary_n = {}
        for line in open(path, errors='replace'):
            if line.startswith('INFBENCH decision '):
                row = dict(re.findall(r'(\w+)=(\S+)', line))
                if 'seconds' in row:
                    rows[(row['policy'], row['mode'])].append(row)
            elif line.startswith('INFBENCH summary '):
                row = dict(re.findall(r'(\w+)=(\S+)', line))
                if 'mode' in row:
                    summary_n[(row['policy'], row['mode'])] = int(row['n'])
            elif line.startswith('INFBENCH tile '):
                row = dict(re.findall(r'(\w+)=(\S+)', line))
                key = (row['policy'], row['mode'])
                n = summary_n.get(key, 0)
                tiles[key][row['tile']] += float(row['mse']) * n
            elif line.startswith('INFBENCH truth '):
                row = dict(re.findall(r'(\w+)=(\S+)', line))
                leaves[row['policy']].append(float(row['prior_probability']))
        for key, n in summary_n.items():
            counts[key] += n
    policies = sorted({policy for policy, _ in rows})
    for policy in policies:
        n = len(rows[(policy, 'prior')])
        print(f'\n== policy {policy}: {n} observations; '
              f'mean prior probability of the true leave {st.mean(leaves[policy]):.4f}')
        print(f'{"mode":16}{"NLL nats":>10}{"NLL bits":>10}{"gain nats":>11}{"+-95%":>8}{"gain bits":>11}'
              f'{"KLV MSE":>10}{"vowel MSE":>11}{"score MSE":>11}{"tile MSE":>10}{"excluded":>10}')
        for mode in MODES:
            data = rows.get((policy, mode))
            if not data:
                continue
            mean = {f: st.mean(float(r[f]) for r in data) for f in FIELDS}
            gains = [float(r['gain_nats']) for r in data]
            half = 1.96 * st.stdev(gains) / math.sqrt(len(gains)) if len(gains) > 1 else float('nan')
            excluded = st.mean(int(r['excluded']) for r in data)
            print(f'{mode:16}{mean["mixed_nll"]:10.3f}{mean["mixed_nll"] / math.log(2):10.3f}'
                  f'{mean["gain_nats"]:11.3f}{half:8.3f}{mean["gain_nats"] / math.log(2):11.3f}'
                  f'{mean["klv_mse"]:10.2f}{mean["vowel_mse"]:11.4f}{mean["score_mse"]:11.3f}'
                  f'{mean["tile_mse"]:10.4f}{100 * excluded:9.1f}%')
        prior, post = tiles.get((policy, 'prior')), tiles.get((policy, 'softnocutofft8'))
        if prior and post and counts[(policy, 'prior')]:
            total = counts[(policy, 'prior')]
            print('per-tile MSE, prior -> softmax T8 (change):')
            parts = []
            for tile in sorted(prior, key=lambda t: -prior[t]):
                a, b = prior[tile] / total, post[tile] / total
                if a > 0:
                    parts.append(f'{tile} {a:.4f}->{b:.4f} ({100 * (b - a) / a:+.0f}%)')
            for start in range(0, len(parts), 5):
                print('  ' + '   '.join(parts[start:start + 5]))


if __name__ == '__main__':
    main()
