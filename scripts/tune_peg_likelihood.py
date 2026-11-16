#!/usr/bin/env python3
"""Tune a solver-based likelihood for pre-endgame leave inference, offline.

Reads INFBENCH pegcache records (one greedy solve per candidate rack) and
scores posteriors of the form  ways * exp(-regret / T), where regret is the
solver utility the observed move gives up against that rack's best candidate
and utility = A * win + spread. Parameters are chosen on even pairs and
reported on odd pairs.
"""
import math
import re
import statistics as st
import sys
from collections import defaultdict

SCORES = dict(zip('ABCDEFGHIJKLMNOPQRSTUVWXYZ?',
                  [1, 3, 3, 2, 1, 4, 2, 4, 1, 8, 5, 1, 3, 1, 1, 3, 10, 1, 1, 1, 1, 4, 4, 8, 4, 10, 0]))
VOWELS = set('AEIOU')
PRIOR_MIX = 0.01


def load(paths):
    observations = defaultdict(list)
    for path in paths:
        for line in open(path, errors='replace'):
            if not line.startswith('INFBENCH pegcache '):
                continue
            row = dict(re.findall(r'(\w+)=(\S+)', line))
            if 'top' not in row:
                continue
            try:
                top = [tuple(map(float, item.split(':'))) for item in row['top'].split(',')]
            except ValueError:
                continue  # a line still being written
            if not line.endswith('\n') or any(len(item) != 2 for item in top):
                continue
            rack = {'leave': row['leave'], 'truth': row['truth'] == '1', 'ways': float(row['ways']),
                    'klv': float(row['klv']), 'rank': int(row['observed_rank']),
                    'win': float(row['observed_win']), 'spread': float(row['observed_spread']),
                    'top': top}
            observations[(path, row['pair'], row['game'], row['turn'])].append(rack)
    return {key: racks for key, racks in observations.items() if sum(r['truth'] for r in racks) == 1}


def regret(rack, win_points):
    if rack['rank'] < 0:
        return float('inf')
    observed = win_points * rack['win'] + rack['spread']
    best = max(win_points * win + spread for win, spread in rack['top'])
    return max(0.0, best - observed)


def posterior(racks, win_points, temperature):
    total_ways = sum(r['ways'] for r in racks)
    prior = [r['ways'] / total_ways for r in racks]
    if temperature is None:
        return prior, prior
    regrets = [regret(r, win_points) for r in racks]
    if temperature == 0:
        weights = [r['ways'] if x == 0 else 0.0 for r, x in zip(racks, regrets)]
    else:
        weights = [r['ways'] * math.exp(-x / temperature) for r, x in zip(racks, regrets)]
    mass = sum(weights)
    if mass <= 0:
        return prior, prior
    return [w / mass for w in weights], prior


def features(leave):
    return (sum(c in VOWELS for c in leave), sum(SCORES[c.upper() if c != '?' else '?'] for c in leave))


def score(observations, keys, win_points, temperature):
    out = defaultdict(list)
    for key in keys:
        racks = observations[key]
        post, prior = posterior(racks, win_points, temperature)
        truth = next(i for i, r in enumerate(racks) if r['truth'])
        mixed = (1 - PRIOR_MIX) * post[truth] + PRIOR_MIX * prior[truth]
        out['nll'].append(-math.log(mixed))
        out['gain'].append(math.log(mixed) - math.log(prior[truth]))
        out['excluded'].append(post[truth] == 0)
        ties = sum(p == max(post) for p in post)
        out['top1'].append(1 / ties if post[truth] == max(post) else 0.0)
        for name, values in (('klv', [r['klv'] for r in racks]),
                             ('vowel', [features(r['leave'])[0] for r in racks]),
                             ('score', [features(r['leave'])[1] for r in racks])):
            expected = sum(p * v for p, v in zip(post, values))
            out[name].append((expected - values[truth]) ** 2)
        tile = 0.0
        for letter in SCORES:
            counts = [r['leave'].count(letter) for r in racks]
            if any(counts):
                expected = sum(p * c for p, c in zip(post, counts))
                tile += (expected - counts[truth]) ** 2
        out['tile'].append(tile)
    return out


def report(label, result):
    gains = result['gain']
    half = 1.96 * st.stdev(gains) / math.sqrt(len(gains)) if len(gains) > 1 else float('nan')
    print(f'{label:34}{st.mean(result["nll"]):8.3f}{st.mean(gains):10.3f}{half:7.3f}'
          f'{st.mean(gains) / math.log(2):9.3f}{st.mean(result["klv"]):9.2f}{st.mean(result["vowel"]):8.4f}'
          f'{st.mean(result["score"]):8.3f}{st.mean(result["tile"]):8.4f}'
          f'{100 * st.mean(result["top1"]):7.1f}%{100 * st.mean(result["excluded"]):7.1f}%')


def main():
    observations = load(sys.argv[1:])
    keys = sorted(observations)
    train = [k for k in keys if int(k[1]) % 2 == 0]
    test = [k for k in keys if int(k[1]) % 2 == 1]
    racks = [len(observations[k]) for k in keys]
    ranks = [next(r for r in observations[k] if r['truth'])['rank'] for k in keys]
    print(f'{len(keys)} observations ({len(train)} tuning, {len(test)} held out); '
          f'mean candidate racks {st.mean(racks):.1f}')
    print(f'solver ranks the observed move first for the true rack in '
          f'{100 * st.mean(r == 0 for r in ranks):.1f}% of observations, top three in '
          f'{100 * st.mean(0 <= r < 3 for r in ranks):.1f}%')
    best = None
    for win_points in (0, 25, 50, 100, 200, 400, 1000, 3000, 10000, 100000):
        for temperature in (0.25, 0.5, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48):
            value = st.mean(score(observations, train, win_points, temperature)['gain'])
            if best is None or value > best[0]:
                best = (value, win_points, temperature)
    _, win_points, temperature = best
    print(f'chosen on the tuning half: A = {win_points} points per win, T = {temperature} points\n')
    print(f'{"held-out half":34}{"NLL":>8}{"gain nats":>10}{"+-95%":>7}{"bits":>9}{"KLV MSE":>9}'
          f'{"vowel":>8}{"score":>8}{"tile":>8}{"top-1":>8}{"excl.":>8}')
    report('prior', score(observations, test, 0, None))
    report('solver hard (regret 0 only)', score(observations, test, win_points, 0))
    report(f'solver soft A={win_points} T={temperature}', score(observations, test, win_points, temperature))
    for other in (temperature / 2, temperature * 2, temperature * 4):
        report(f'solver soft A={win_points} T={other:g}', score(observations, test, win_points, other))
    report('spread only, T=8', score(observations, test, 0, 8))


if __name__ == '__main__':
    main()
