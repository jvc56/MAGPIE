#!/usr/bin/env python3
"""Summarize infbench staticN logs; cluster quality differences by game pair."""
import argparse
import json
import math
from collections import defaultdict
from pathlib import Path
from statistics import mean

METRICS = ('nll', 'klv_mse', 'vowel_mse', 'score_mse', 'tile_mse', 'duplicate_mse')


def summarize(paths):
    rows = defaultdict(dict)
    exchanged = defaultdict(dict)
    for path in paths:
        settings = None
        for line in path.read_text().splitlines():
            if line.startswith('STATICCAL_SETTINGS '):
                settings = dict(item.split('=', 1) for item in line.split()[1:])
            is_exchanged = line.startswith('STATICCAL_EXCHANGED ')
            if not is_exchanged and not line.startswith('STATICCAL '):
                continue
            row = dict(item.split('=', 1) for item in line.split()[1:])
            pair = int(settings['seed']) + int(row['pair'])
            key = (pair, int(row['game']), int(row['turn']))
            temperature = int(row['temperature'])
            if is_exchanged:
                exchanged[key][temperature] = {name: float(row[name]) for name in METRICS[1:]}
                continue
            if temperature in rows[key]:
                raise ValueError(f'duplicate observation: {path} {key}')
            rows[key][temperature] = {name: float(row[name]) for name in (*METRICS, 'seconds')}
    complete = {key: modes for key, modes in rows.items() if len(modes) == 7}
    result = {'observations': len(complete), 'incomplete': len(rows) - len(complete), 'models': {}}
    for temperature in (3, 5, 7, 8, 9, 10, 15):
        if not complete:
            break
        values = [modes[temperature] for modes in complete.values()]
        times = sorted(value['seconds'] for value in values)
        model = {metric: mean(value[metric] for value in values) for metric in METRICS}
        model['time_seconds'] = {'average': mean(times), 'p95': times[math.ceil(.95 * len(times)) - 1], 'max': max(times)}
        differences = defaultdict(list)
        for key, modes in complete.items():
            differences[key[0]].append(modes[temperature]['nll'] - modes[8]['nll'])
        delta = mean(value for group in differences.values() for value in group)
        count = len(differences)
        half = (1.96 * math.sqrt(count / (count - 1) * sum(sum(value - delta for value in group) ** 2 for group in differences.values())) / len(values)) if count > 1 else None
        model['nll_delta_vs_t8'] = {'mean': delta, 'ci95_half_width': half, 'pairs': count}
        exchange_values = [exchanged[key][temperature] for key in complete if key in exchanged and temperature in exchanged[key]]
        if exchange_values:
            model['exchanged'] = {metric: mean(value[metric] for value in exchange_values) for metric in METRICS[1:]}
            model['exchanged']['observations'] = len(exchange_values)
        result['models'][str(temperature)] = model
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    text = json.dumps(summarize(args.logs), indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    main()
