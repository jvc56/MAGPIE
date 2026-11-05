#!/usr/bin/env python3
"""Fit inference settings on fixed training pairs; score untouched validation pairs."""
import argparse
import collections
import json
import math
from pathlib import Path
import statistics

METRICS = ('nll', 'klv_mse', 'vowel_mse', 'score_mse', 'tile_mse', 'excluded', 'empty')


def stage(bag):
    return '1-14' if bag <= 14 else '15-42' if bag <= 42 else '43+'


def group_key(obs):
    return ('exchange' if obs['exchange'] else 'play', obs['leave'], stage(obs['bag']))


def score(obs, setting):
    model, margin, temperature = setting
    if model == 'hybrid':
        model = 'pat' if not obs['exchange'] and obs['leave'] <= 4 else 'static'
    return obs['scores'][(model, margin, temperature)]


def best_setting(observations, candidates):
    return min(candidates, key=lambda setting: statistics.mean(score(obs, setting)['nll'] for obs in observations))


def describe(setting):
    return dict(model=setting[0], margin=setting[1], temperature=setting[2])


def aggregate(observations, selector):
    return {metric: statistics.mean(score(obs, selector(obs))[metric] for obs in observations)
            for metric in METRICS} if observations else {}


def paired_delta(observations, selector, baseline):
    pairs = collections.defaultdict(list)
    for obs in observations:
        pairs[obs['pair']].append(score(obs, selector(obs))['nll'] - score(obs, baseline)['nll'])
    values = [statistics.mean(deltas) for deltas in pairs.values()]
    if len(values) < 2:
        return {'pairs': len(values)}
    mean = statistics.mean(values)
    half = 1.959964 * statistics.stdev(values) / math.sqrt(len(values))
    return {'pairs': len(values), 'mean_nll_difference': mean, 'ci95': [mean-half, mean+half]}


def analyze(directory):
    manifest = json.loads((directory/'manifest.json').read_text())
    records = {}
    lines = (directory/'observations.log').read_text().splitlines()
    if 'supplemental_exchanges' in manifest:
        lines += (directory/'exchange-observations.log').read_text().splitlines()
    for line in lines:
        if not line.startswith('CALIBRATION '):
            continue
        row = dict(item.split('=') for item in line.split()[1:])
        key = tuple(int(row[name]) for name in ('pair', 'game', 'turn'))
        obs = records.setdefault(key, {'pair': key[0], 'leave': int(row['leave']),
                                      'bag': int(row['bag']), 'exchange': bool(int(row['exchange'])), 'scores': {}})
        setting = (row['model'], int(row['margin']), int(row['temperature']))
        if setting in obs['scores']:
            raise ValueError(f'Duplicate observation/mode: {key} {setting}')
        obs['scores'][setting] = {metric: float(row[metric]) for metric in METRICS}
    observations = list(records.values())
    training_end = manifest['training_pair_indices'][1]
    primary_end = manifest['validation_pair_indices'][1]
    extra = manifest.get('supplemental_exchanges', {})
    extra_train = extra.get('training_indices', [-1, -1])
    train = [obs for obs in observations if obs['pair'] <= training_end or extra_train[0] <= obs['pair'] <= extra_train[1]]
    primary_train = [obs for obs in train if obs['pair'] <= training_end]
    training_pairs = {obs['pair'] for obs in train}
    all_valid = [obs for obs in observations if obs['pair'] not in training_pairs]
    valid = [obs for obs in all_valid if obs['pair'] <= primary_end]
    exchange_valid = [obs for obs in all_valid if obs['exchange']] 
    candidates = [('prior', 0, 0)] + [(model, margin, temp) for model in ('static', 'hybrid')
        for margin in manifest['margins'] for temp in manifest['temperatures']]
    # Missing rows are a failed/incomplete experiment, not data to silently drop.
    for obs in observations:
        for setting in candidates:
            score(obs, setting)
    baseline = ('static', 20, 5)
    hybrid_baseline = ('hybrid', 20, 5)
    global_static = best_setting(primary_train, [s for s in candidates if s[0] != 'hybrid'])
    global_hybrid = best_setting(primary_train, [s for s in candidates if s[0] != 'static'])
    global_best = best_setting(primary_train, candidates)
    selections = {}
    group_reports = []
    for key in sorted({group_key(obs) for obs in observations}):
        source, selected_train = 'global', primary_train
        for depth, label in ((3, 'type+leave+stage'), (2, 'type+leave'), (1, 'type')):
            subset = [obs for obs in train if group_key(obs)[:depth] == key[:depth]]
            if (len(subset) >= manifest['minimum_training_observations'] and
                    len({obs['pair'] for obs in subset}) >= manifest['minimum_training_pairs']):
                source, selected_train = label, subset
                break
        selected = best_setting(selected_train, candidates)
        if key[1] == 0:
            source, selected = "deterministic-empty-leave", ("prior", 0, 0)
        selections[key] = selected
        cell_train = [obs for obs in train if group_key(obs) == key]
        cell_valid = [obs for obs in all_valid if group_key(obs) == key]
        group_reports.append({'type': key[0], 'leave': key[1], 'bag': key[2],
            'train_n': len(cell_train), 'validation_n': len(cell_valid), 'source': source,
            'fit_n': len(selected_train), 'selected': describe(selected),
            'validation_baseline': aggregate(cell_valid, lambda _: baseline),
            'validation_tuned': aggregate(cell_valid, lambda _: selected)})
    selectors = {'prior': lambda _: ('prior', 0, 0),
                 'static20t5': lambda _: baseline, 'pat1to4_20t5': lambda _: hybrid_baseline,
                 'global_static_tuned': lambda _: global_static,
                 'global_hybrid_tuned': lambda _: global_hybrid,
                 'global_best_tuned': lambda _: global_best,
                 'stratified_tuned': lambda obs: selections[group_key(obs)]}
    result = {'training_observations': len(train), 'validation_observations': len(valid),
        'training_exchange_observations': sum(obs['exchange'] for obs in train),
        'validation_exchange_observations': len(exchange_valid),
        'exchange_validation': {name: {'n': len(exchange_valid), 'metrics': aggregate(exchange_valid, selector),
            'paired_nll_vs_static20t5': paired_delta(exchange_valid, selector, baseline)} for name, selector in selectors.items()},
        'global_static': describe(global_static), 'global_hybrid': describe(global_hybrid),
        'global_best': describe(global_best),
        'validation': {name: {'metrics': aggregate(valid, selector),
                             'paired_nll_vs_static20t5': paired_delta(valid, selector, baseline),
                             'paired_nll_vs_global_best': paired_delta(valid, selector, global_best)}
                       for name, selector in selectors.items()}, 'groups': group_reports,
        'pat_by_leave': []}
    for leave in range(1, 5):
        subset = [obs for obs in valid if not obs['exchange'] and obs['leave'] == leave]
        result['pat_by_leave'].append({'leave': leave, 'n': len(subset),
            'static20t5': aggregate(subset, lambda _: baseline),
            'pat20t5': aggregate(subset, lambda _: hybrid_baseline),
            'paired_nll_difference': paired_delta(subset, lambda _: hybrid_baseline, baseline)})
    (directory/'calibration-results.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = analyze(args.directory)
    print(json.dumps({key: value for key, value in result.items() if key != 'groups'}, indent=2))
