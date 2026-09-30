#!/usr/bin/env python3
"""Paired comparisons and integrity checks for the completed research run."""
import csv
import json
import math
import random
import statistics
from pathlib import Path

D = Path('/Users/olaugh/sources/magpie-pat-postx/postx/threatgames/pass_relative_20260929')
OLD = D.parent / 'extended_2h40'


def read(files):
    data = {}
    for file in files:
        for row in csv.DictReader(file.open()):
            assert None not in row, file
            data.setdefault(int(row['pos']), {}).setdefault(row['arm'], {})[row['move']] = row
    return data


def chosen(rows):
    selected = [move for move, row in rows.items() if int(row['chosen'])]
    assert len(selected) == 1
    return selected[0]


def mean_se(values):
    return {'n': len(values), 'mean': statistics.mean(values) if values else None,
            'se': statistics.stdev(values) / math.sqrt(len(values)) if len(values) > 1 else None}


def bootstrap(gains, meta):
    if not gains:
        return None
    groups = {}
    for pos, value in gains.items():
        groups.setdefault(meta.get(pos, {}).get('game', str(pos)), []).append(value)
    groups = list(groups.values())
    if len(groups) < 2:
        return None
    rng = random.Random(2026093001)
    means = []
    for _ in range(5000):
        sample = rng.choices(groups, k=len(groups))
        means.append(sum(map(sum, sample)) / sum(map(len, sample)))
    means.sort()
    return [means[int(.025 * len(means))], means[int(.975 * len(means))]]


def compare(left, left_arm, right, right_arm, refs, positions, meta):
    gains = {}
    eqgains = []
    changed = 0
    win = loss = tie = 0
    for pos in positions:
        lm, rm = chosen(left[pos][left_arm]), chosen(right[pos][right_arm])
        lr, rr = refs[pos]['reference'][lm], refs[pos]['reference'][rm]
        gains[pos] = 100 * (float(lr['sim_wp']) - float(rr['sim_wp']))
        eqgains.append(float(lr['sim_eq']) - float(rr['sim_eq']))
        changed += lm != rm
        win += gains[pos] > 0
        loss += gains[pos] < 0
        tie += gains[pos] == 0
    return {'left': left_arm, 'right': right_arm, 'wp_gain_pp': mean_se(list(gains.values())),
            'game_bootstrap95_pp': bootstrap(gains, meta), 'equity_gain': mean_se(eqgains),
            'different_choices': changed, 'positive': win, 'negative': loss, 'zero': tie}


def audit(data, budget=15000, reference_budget=60000):
    count = rows_count = reused = 0
    for pos, arms in data.items():
        for arm, rows in arms.items():
            count += 1
            assert len(rows) == int(next(iter(rows.values()))['candidates']), (pos, arm)
            chosen(rows)
            wall = float(next(iter(rows.values()))['wall_ms'])
            if arm == 'reference':
                assert wall >= reference_budget - 1000, (pos, arm, wall)
            elif wall < 1:
                reused += 1
            else:
                assert wall >= budget - 100, (pos, arm, wall)
            for row in rows.values():
                rows_count += 1
                for key in ['sim_wp', 'sim_wp_sem', 'sim_eq', 'sim_eq_sem']:
                    assert math.isfinite(float(row[key])), (pos, arm, key)
                assert 0 <= float(row['sim_wp']) <= 1
                assert float(row['sim_wp_sem']) >= 0 and float(row['sim_eq_sem']) >= 0
    return {'positions': len(data), 'pools': count, 'rows': rows_count,
            'identical_pool_reuse': reused, 'full_budget_and_finite_results': True}


def audit_checks(files, racks, reader=None):
    records = {}
    for file in files:
        for row in reader(file) if reader else csv.DictReader(file.open()):
            records[int(row['pos']), row['move']] = row
    pass_means = {}
    terminal = {}
    for (pos, move), row in records.items():
        assert int(row['racks']) == racks and int(row['conditioned']) == 1
        values = {key: float(row[key]) for key in
                  ['pass_reply_mean', 'candidate_reply_mean', 'blocking_delta', 'blocking_adjustment',
                   'pass_followup_mean', 'candidate_followup_mean', 'setup_delta', 'setup_adjustment']}
        assert all(map(math.isfinite, values.values()))
        assert abs(values['blocking_delta'] - (values['pass_reply_mean'] - values['candidate_reply_mean'])) < 1e-7
        assert abs(values['setup_delta'] - (values['candidate_followup_mean'] - values['pass_followup_mean'])) < 1e-7
        assert abs(values['blocking_adjustment'] - 1.4 * values['blocking_delta']) < 1e-7
        assert abs(values['setup_adjustment'] - .75 * values['setup_delta']) < 1e-7
        assert abs(pass_means.setdefault(pos, values['pass_reply_mean']) - values['pass_reply_mean']) < 1e-7
        if move.startswith('ex '):
            assert values['blocking_delta'] == 0 and values['setup_delta'] == 0
        ended = int(row.get('terminal_replies', 0))
        assert 0 <= ended <= racks
        if ended:
            terminal.setdefault(pos, {})[move] = ended
    return {'positions': len(pass_means), 'candidate_records': len(records), 'racks': racks,
            'paired_means_weights_and_exchange_zeros_match': True, 'terminal_reply_cases': terminal}


def main():
    assert (D / 'complete.json').exists(), 'The full study and deep validation must finish first.'
    primary = read(sorted(D.glob('w[0-7].csv')) + [D / 'r0.csv'])
    legacy = read(sorted(OLD.glob('w[0-7].csv')))
    width = read(sorted(D.glob('v[0-7].csv')))
    matched = read(sorted(D.glob('m[0-7].csv')))
    sample = read(sorted(D.glob('c[0-7].csv')))
    six = read(sorted(D.glob('d[0-7].csv')))
    quota = read(sorted((D / 'tile_followup').glob('w[0-3].csv')))
    assert [len(data) for data in [primary, width, matched, sample, six, quota]] == [320, 128, 128, 128, 64, 40]
    meta = {int(row['pos']): row for row in csv.DictReader((D / 'positions.meta.csv').open())}
    qmeta = {int(row['pos']): row for row in csv.DictReader((D / 'tile_followup' / 'positions.meta.csv').open())}
    out = {'audit': {name: audit(data, reference_budget=60000) for name, data in
                     [('primary', primary), ('width', width), ('matched', matched),
                      ('sample128', sample), ('six_ply', six), ('quota', quota)]},
           'matched_width': {}, 'width': {}, 'six_ply': {}, 'legacy_choice_comparison': {},
           'quota': {}, 'novel_choice_breakdown': {}, 'primary_strata': {},
           'sample128_vs64': {}, 'six_vs_four_ply_choices': {}}
    out['diagnostic_audit'] = {
        'primary': audit_checks(sorted(D.glob('w[0-7]_checks.csv')) + [D / 'r0_checks.csv'], 64),
        'width': audit_checks(sorted(D.glob('v[0-7]_checks.csv')), 64),
        'six_ply': audit_checks(sorted(D.glob('d[0-7]_checks.csv')), 64),
        'sample128': audit_checks(sorted(D.glob('c[0-7]_checks.csv')), 128),
        'quota': audit_checks(sorted((D / 'tile_followup').glob('w[0-3]_checks.csv')), 64)}
    strata = {'early_bag60plus': [], 'middle_bag30to59': [], 'late_bag7to29': [],
              'competitive': [], 'tail': []}
    for pos in sorted(primary):
        if pos >= 6000:
            continue
        bag = int(next(iter(primary[pos]['reference'].values()))['bag'])
        assert bag >= 7
        phase = 'early_bag60plus' if bag >= 60 else 'middle_bag30to59' if bag >= 30 else 'late_bag7to29'
        strata[phase].append(pos)
        base_wp = float(primary[pos]['reference'][chosen(primary[pos]['static25'])]['sim_wp'])
        strata['competitive' if .01 < base_wp < .99 else 'tail'].append(pos)
    for phase, positions in strata.items():
        out['primary_strata'][phase] = {}
        for arm in ['threat_wide', 'setup_wide', 'signals', 'exchange', 'combined']:
            result = compare(primary, arm, primary, 'static25', primary, positions, meta)
            result['choices_outside25'] = sum(chosen(primary[pos][arm]) not in primary[pos]['static25'] for pos in positions)
            result['mean_candidates'] = statistics.mean(len(primary[pos][arm]) for pos in positions)
            out['primary_strata'][phase][arm] = result
    for arm, control in [('setup_wide', 'static_setup_count'), ('combined', 'static_combined_count')]:
        positions = sorted(matched)
        assert all(len(primary[pos][arm]) == len(matched[pos][control]) for pos in positions)
        out['matched_width'][arm] = compare(primary, arm, matched, control, primary, positions, meta)
        out['matched_width'][arm]['mean_candidates'] = statistics.mean(len(primary[pos][arm]) for pos in positions)
        out['matched_width'][arm]['by_phase'] = {
            phase: compare(primary, arm, matched, control, primary,
                           [pos for pos in positions if (pos < 6000) == nonopening], meta)
            for phase, nonopening in [('nonopening', True), ('opening', False)]}
    for name, data, refs in [('width', width, primary), ('six_ply', six, six)]:
        for arm in ['adaptive_static', 'static60', 'adaptive_signals']:
            out[name][arm] = compare(data, arm, data, 'static25_fulltime', refs, sorted(data), meta)
            out[name][arm]['by_phase'] = {
                phase: compare(data, arm, data, 'static25_fulltime', refs,
                               [pos for pos in sorted(data) if (pos < 6000) == nonopening], meta)
                for phase, nonopening in [('nonopening', True), ('opening', False)]}
        out[name]['signals_vs_adaptive_static'] = compare(data, 'adaptive_signals', data, 'adaptive_static', refs, sorted(data), meta)
        out[name]['signals_vs_adaptive_static']['by_phase'] = {
            phase: compare(data, 'adaptive_signals', data, 'adaptive_static', refs,
                           [pos for pos in sorted(data) if (pos < 6000) == nonopening], meta)
            for phase, nonopening in [('nonopening', True), ('opening', False)]}
        out[name]['signals_vs_static60'] = compare(data, 'adaptive_signals', data, 'static60', refs, sorted(data), meta)
        out[name]['signals_vs_static60']['by_phase'] = {
            phase: compare(data, 'adaptive_signals', data, 'static60', refs,
                           [pos for pos in sorted(data) if (pos < 6000) == nonopening], meta)
            for phase, nonopening in [('nonopening', True), ('opening', False)]}
    for arm, sampled_arm in [('setup_wide', 'setup_conditioned'), ('signals', 'signals_conditioned'), ('combined', 'combined_conditioned')]:
        out['sample128_vs64'][arm] = compare(sample, sampled_arm, primary, arm, primary, sorted(sample), meta)
        out['sample128_vs64'][arm]['by_phase'] = {
            phase: compare(sample, sampled_arm, primary, arm, primary,
                           [pos for pos in sorted(sample) if (pos < 6000) == nonopening], meta)
            for phase, nonopening in [('nonopening', True), ('opening', False)]}
    assert set(six) <= set(width)
    for arm in ['static25_fulltime', 'adaptive_static', 'static60', 'adaptive_signals']:
        out['six_vs_four_ply_choices'][arm] = compare(six, arm, width, arm, six, sorted(six), meta)
        out['six_vs_four_ply_choices'][arm]['by_phase'] = {
            phase: compare(six, arm, width, arm, six,
                           [pos for pos in sorted(six) if (pos < 6000) == nonopening], meta)
            for phase, nonopening in [('nonopening', True), ('opening', False)]}
    for arm in ['threat_wide', 'setup_wide', 'signals', 'exchange', 'combined']:
        positions = sorted(primary)
        assert all(chosen(legacy[pos][arm]) in primary[pos]['reference'] for pos in positions)
        out['legacy_choice_comparison'][arm] = compare(primary, arm, legacy, arm, primary, positions, meta)
        novel = [pos for pos in positions if chosen(primary[pos][arm]) not in primary[pos]['static25']]
        out['novel_choice_breakdown'][arm] = compare(primary, arm, primary, 'static25', primary, novel, meta)
    for arm in ['tile_quota', 'tile_exchange']:
        out['quota'][arm] = compare(quota, arm, quota, 'static25_fulltime', quota, sorted(quota), qmeta)
    deep_primary = sorted(D.glob('new*_all25_p16.csv'))
    deep_quota = sorted((D / 'tile_followup').glob('validate*_p16.csv'))
    out['deep_audit'] = {}
    if deep_primary and deep_quota:
        out['deep_audit'] = {'primary': audit(read(deep_primary), reference_budget=600000),
                             'quota': audit(read(deep_quota), reference_budget=600000)}
        admission_main = read(sorted(D.glob('admission*_p16.csv')))
        admission_quota = read(sorted((D / 'tile_followup').glob('admission*_p16.csv')))
        out['deep_admission_audit'] = {'primary': audit(admission_main), 'quota': audit(admission_quota)}
        out['deep_admission'] = []
        for name, selections, refs in [('primary', admission_main, read(deep_primary)),
                                        ('quota', admission_quota, read(deep_quota))]:
            for pos, arms in selections.items():
                armnames = list(arms)
                assert len(armnames) == 2, (pos, armnames)
                base = 'static25_fulltime'
                enlarged = 'static25_admitted'
                assert set(armnames) == {base, enlarged}
                result = compare(selections, enlarged, selections, base, refs, [pos], meta if name == 'primary' else qmeta)
                result.update({'dataset': name, 'pos': pos, 'base_move': chosen(arms[base]),
                               'enlarged_move': chosen(arms[enlarged]), 'candidates': len(arms[enlarged])})
                out['deep_admission'].append(result)
    destination = Path.cwd() / 'pass-relative-controls-analysis.json'
    destination.write_text(json.dumps(out, indent=2) + '\n')
    print(json.dumps(out, indent=2))


if __name__ == '__main__':
    main()
