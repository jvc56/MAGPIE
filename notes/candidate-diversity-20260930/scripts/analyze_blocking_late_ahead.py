#!/usr/bin/env python3
"""Exploratory score-conditioned analysis of completed overnight data only."""
import json
import math
import statistics
from pathlib import Path

from overnight_analyze import interval, t_two_sided
from overnight_prepare import D, save
from overnight_run import csv_rows, read_text_retry


def summarize(rows):
    gains = [row['wp_gain_pp'] for row in rows]
    mean = statistics.mean(gains) if gains else None
    se = statistics.stdev(gains) / math.sqrt(len(gains)) if len(gains) > 1 else None
    return {'positions': len(gains), 'mean_gain_pp': mean,
            'bootstrap95_pp': interval(gains),
            'two_sided_p': t_two_sided(mean / se, len(gains) - 1) if se else None,
            'different_choices': sum(row['left_move'] != row['right_move'] for row in rows),
            'positive': sum(value > 0 for value in gains),
            'negative': sum(value < 0 for value in gains),
            'zero': sum(value == 0 for value in gains)}


def main():
    assert (D / 'complete.json').exists()
    summary = json.loads(read_text_retry(D / 'summary.json'))
    metadata = {int(row['pos']): row for row in csv_rows(D / 'positions.meta.csv')}
    scores = {}
    for line in read_text_retry(D / 'positions.cgp').splitlines():
        identifier, cgp = line.split(',', 1)
        scores[int(identifier)] = tuple(map(int, cgp.split()[2].split('/')))
    all_rows = summary['exploratory']['threat_wide']['per_position']
    late = [row for row in all_rows if metadata[row['pos']]['phase'] == 'late']
    ahead = [row for row in late if scores[row['pos']][0] > scores[row['pos']][1]]
    behind = [row for row in late if scores[row['pos']][0] < scores[row['pos']][1]]
    by_pos = {row['pos']: row for row in ahead}
    reference = {}; blocking = {}
    for file in sorted(D.glob('w[0-7].csv')):
        for row in csv_rows(file):
            pos = int(row['pos'])
            if pos not in by_pos:
                continue
            if row['arm'] == 'reference':
                reference.setdefault(pos, {})[row['move']] = row
            elif row['arm'] == 'threat_wide':
                blocking.setdefault(pos, {})[row['move']] = row
    cases = []
    for pos, row in sorted(by_pos.items()):
        if row['left_move'] == row['right_move']:
            continue
        ref = reference[pos]; left = ref[row['left_move']]; right = ref[row['right_move']]
        count = len(blocking[pos]); deepfile = D / f'new{pos}_all25_p16.csv'
        deep = {record['move']: record for record in csv_rows(deepfile)} if deepfile.exists() else {}
        deep_gain = None; deep_sem = None; deep_prefix_gain = None
        if deep:
            dl = deep[row['left_move']]; dr = deep[row['right_move']]
            deep_gain = 100 * (float(dl['sim_wp']) - float(dr['sim_wp']))
            deep_sem = 100 * (float(dl['sim_wp_sem']) + float(dr['sim_wp_sem']))
            prefix = [move for move, record in ref.items() if int(record['static_rank']) <= count]
            assert all(move in deep for move in prefix)
            deep_prefix_gain = 100 * (float(dl['sim_wp']) - max(float(deep[move]['sim_wp']) for move in prefix))
        cases.append({'pos': pos, 'score_lead': scores[pos][0] - scores[pos][1],
                      'blocking_choice': row['left_move'], 'static25_choice': row['right_move'],
                      'static_rank': int(left['global_static_rank']), 'blocking_pool_count': count,
                      'outside_static25': int(left['static_rank']) > 25,
                      'outside_same_size_static_prefix': int(left['static_rank']) > count,
                      'baseline_reference_wp_percent': 100 * float(right['sim_wp']),
                      'four_ply_gain_pp': row['wp_gain_pp'],
                      'sixteen_ply_gain_pp': deep_gain,
                      'sixteen_ply_gain_sem_upper_pp': deep_sem,
                      'sixteen_ply_gain_vs_best_same_size_static_pp': deep_prefix_gain})
    basewps = [float(reference[pos][row['right_move']]['sim_wp']) for pos, row in by_pos.items()]
    close = [row for row in ahead if 0 < scores[row['pos']][0] - scores[row['pos']][1] <= 50]
    output = {'scope': 'User-requested exploratory subgroup after the overnight study; completed data only. No new simulations.',
              'definition': {'late': '7 to 29 tiles in bag', 'ahead': 'on-turn score strictly exceeds opponent score'},
              'comparison': 'Original 15-second blocking pool versus original 15-second static25 pool, evaluated in the independent 60-second four-ply reference.',
              'late_ahead': summarize(ahead), 'late_behind': summarize(behind), 'all_late': summarize(late),
              'context': {'median_score_lead': statistics.median(scores[pos][0] - scores[pos][1] for pos in by_pos),
                          'median_baseline_reference_wp_percent': 100 * statistics.median(basewps),
                          'baseline_wp_at_least_99_percent': sum(wp >= .99 for wp in basewps),
                          'baseline_wp_at_least_95_percent': sum(wp >= .95 for wp in basewps),
                          'lead_1_to_50_positions': len(close),
                          'lead_1_to_50_different_choices': sum(row['left_move'] != row['right_move'] for row in close)},
              'changed_cases': cases,
              'limits': ['Exploratory, after observing the main study; not a new confirmatory test.',
                         'Blocking pools are larger than static25; no equal-size timed blocking control was run.',
                         'The original sample excludes bag sizes 0 to 6 and empty-bag endgames.',
                         'Longer references exist for five of the six changed cases, selected by the original novel-choice rule.',
                         'Same-size prefix comparisons in longer references are candidate-coverage diagnostics using the best reference-valued play, not measured 15-second control choices.',
                         'Position-level static-rollout sim estimates do not measure played-game strength.']}
    target = D / 'exploratory-blocking-late-ahead'
    target.mkdir(exist_ok=True)
    save(target / 'summary.json', output)
    print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()
