#!/usr/bin/env python3
"""Audit and evaluate the frozen overnight paired confirmation study."""
import csv
import hashlib
import html
import json
import math
import random
import statistics

from analyze_pass_relative_controls import audit, audit_checks, chosen
from overnight_prepare import BIN, D, save
from overnight_run import csv_rows, full, load, read_bytes_retry, read_text_retry


def beta_fraction(a, b, x):
    tiny = 1e-300
    c = 1.0; d = 1.0 - (a + b) * x / (a + 1)
    if abs(d) < tiny:
        d = tiny
    d = 1 / d; result = d
    for index in range(1, 10001):
        aa = index * (b - index) * x / ((a + 2 * index - 1) * (a + 2 * index))
        d = 1 + aa * d; c = 1 + aa / c
        if abs(d) < tiny:
            d = tiny
        if abs(c) < tiny:
            c = tiny
        d = 1 / d; result *= d * c
        aa = -(a + index) * (a + b + index) * x / ((a + 2 * index) * (a + 2 * index + 1))
        d = 1 + aa * d; c = 1 + aa / c
        if abs(d) < tiny:
            d = tiny
        if abs(c) < tiny:
            c = tiny
        d = 1 / d; delta = d * c; result *= delta
        if abs(delta - 1) < 3e-14:
            return result
    raise ArithmeticError('Incomplete beta continued fraction did not converge')


def beta_regularized(a, b, x):
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    coefficient = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
                           + a * math.log(x) + b * math.log1p(-x))
    if x < (a + 1) / (a + b + 2):
        return coefficient * beta_fraction(a, b, x) / a
    return 1 - coefficient * beta_fraction(b, a, 1 - x) / b


def t_two_sided(t, df):
    return beta_regularized(df / 2, .5, df / (df + t * t))


def interval(values):
    if len(values) < 2:
        return None
    rng = random.Random(202609300704)
    samples = sorted(statistics.mean(rng.choices(values, k=len(values))) for _ in range(10000))
    return [samples[250], samples[9750]]


def comparison(left, left_arm, right, right_arm, refs, positions):
    values = []; eqvalues = []; details = []; reference_variances = []
    for pos in positions:
        leftmove = chosen(left[pos][left_arm]); rightmove = chosen(right[pos][right_arm])
        lr = refs[pos]['reference'][leftmove]; rr = refs[pos]['reference'][rightmove]
        gain = 100 * (float(lr['sim_wp']) - float(rr['sim_wp']))
        values.append(gain); eqvalues.append(float(lr['sim_eq']) - float(rr['sim_eq']))
        reference_variances.append(0 if leftmove == rightmove else (100 * (float(lr['sim_wp_sem']) + float(rr['sim_wp_sem']))) ** 2)
        details.append({'pos': pos, 'left_move': leftmove, 'right_move': rightmove, 'wp_gain_pp': gain})
    mean = statistics.mean(values) if values else None
    se = statistics.stdev(values) / math.sqrt(len(values)) if len(values) > 1 else None
    t = mean / se if se else None
    return {'left': left_arm, 'right': right_arm, 'positions': len(values), 'mean_wp_gain_pp': mean,
            'position_se_pp': se, 'bootstrap95_pp': interval(values),
            't_statistic': t, 't_test_two_sided_p': t_two_sided(t, len(values) - 1) if t is not None else 1.0,
            'reference_mc_sem_upper_mean_pp': math.sqrt(sum(reference_variances)) / len(values) if values else None,
            'mean_equity_gain': statistics.mean(eqvalues) if eqvalues else None,
            'different_choices': sum(row['left_move'] != row['right_move'] for row in details),
            'positive': sum(value > 0 for value in values), 'negative': sum(value < 0 for value in values),
            'zero': sum(value == 0 for value in values), 'per_position': details}


def main():
    assert (D / 'data_complete.json').exists()
    for t in [.1, 1, 10]:
        assert abs(t_two_sided(t, 1) - (1 - 2 * math.atan(t) / math.pi)) < 1e-12
        assert abs(t_two_sided(t, 2) - (1 - t / math.sqrt(t * t + 2))) < 1e-12
    completion = json.loads(read_text_retry(D / 'data_complete.json'))
    protocol = json.loads(read_text_retry(D / 'PROTOCOL.json'))
    assert hashlib.sha256(read_bytes_retry(BIN)).hexdigest() == protocol['binary_sha256']
    hashes = json.loads(read_text_retry(D / 'input_hashes.json'))
    assert all(hashlib.sha256(read_bytes_retry(D / path)).hexdigest() == value for path, value in hashes.items())
    primary_all = load(D.glob('w[0-7].csv')); matched_all = load([D / 'matched_effective.csv'])
    primary = {pos: arms for pos, arms in primary_all.items() if full(arms, ['static25', 'threat_wide', 'setup_wide', 'signals', 'exchange', 'combined', 'reference'])}
    matched = {pos: arms for pos, arms in matched_all.items() if full(arms, ['static_setup_count', 'static_combined_count'])}
    positions = sorted(set(primary) & set(matched))
    assert len(positions) == completion['paired_positions']
    meta = {int(row['pos']): row for row in csv_rows(D / 'positions.meta.csv')}
    assert len(meta) == 1200 and len({row['source_seed'] for row in meta.values()}) == 1200
    if completion['full_confirmation_sample']:
        assert len(positions) == 1200
    audits = {'primary': audit(primary), 'matched': audit(matched),
              'checks': audit_checks(sorted(D.glob('w[0-7]_checks.csv')), 64, csv_rows),
              'input_hashes_and_frozen_binary_match': True, 'independent_source_games': 1200,
              'paired_count_matches': True}
    for entry in json.loads(read_text_retry(D / 'matched_identical_reuse.json')):
        pos = entry['pos']; arm = entry['arm']; source = entry['source_arm']
        assert set(matched[pos][arm]) == set(primary[pos][source])
        assert chosen(matched[pos][arm]) == chosen(primary[pos][source])
    audits['identical_matched_pools_reuse_original_full_budget_result'] = True
    check_records = {}
    for file in sorted(D.glob('w[0-7]_checks.csv')):
        for row in csv_rows(file):
            check_records[int(row['pos']), row['move']] = row
    feature_records = {(int(row['pos']), row['move']): row for row in csv_rows(D / 'candidate_features.csv')}
    for pos in primary:
        for move, row in primary[pos]['reference'].items():
            feature = feature_records[pos, move]; check = check_records[pos, move]
            assert int(feature['global_static_rank']) == int(row['global_static_rank'])
            assert abs(float(feature['static_eq']) - float(row['static_eq'])) < 1e-7
            assert abs(2 * (float(row['threat_value']) - float(row['static_eq'])) - float(check['blocking_adjustment'])) < 2e-6
            assert abs(3 * (float(row['setup_value']) - float(row['static_eq'])) - float(check['setup_adjustment'])) < 2e-6
    for pos in positions:
        assert len(primary[pos]['setup_wide']) == len(matched[pos]['static_setup_count'])
        assert set(matched[pos]['static_setup_count']) == {move for move, row in primary[pos]['reference'].items()
                                                        if int(row['static_rank']) <= len(primary[pos]['setup_wide'])}
    primary_result = comparison(primary, 'setup_wide', matched, 'static_setup_count', primary, positions)
    primary_result['statistically_significant_primary'] = bool(completion['full_confirmation_sample'] and primary_result['t_test_two_sided_p'] < .05)
    excluded_combined = {row['pos'] for row in json.loads(read_text_retry(D / 'combined_count_exceptions.json'))}
    combined_positions = [pos for pos in positions if pos not in excluded_combined]
    exploratory = {arm: comparison(primary, arm, primary, 'static25', primary, positions)
                   for arm in ['threat_wide', 'setup_wide', 'signals', 'exchange', 'combined']}
    exploratory['combined_same_count'] = comparison(primary, 'combined', matched, 'static_combined_count', primary, combined_positions)
    phases = {phase: comparison(primary, 'setup_wide', matched, 'static_setup_count', primary,
                               [pos for pos in positions if meta[pos]['phase'] == phase]) for phase in ['early', 'middle', 'late']}
    deep = load([D / f'new{pos}_all25_p16.csv' for pos in completion['deep_completed']])
    deep_audit = audit(deep, reference_budget=600000) if deep else {'positions': 0}
    deep_setup = comparison(primary, 'setup_wide', matched, 'static_setup_count', deep, sorted(deep)) if deep else None
    deep_examples = []
    for pos in deep:
        ref = deep[pos]['reference']; best25 = max(primary[pos]['static25'], key=lambda move: float(ref[move]['sim_wp']))
        bestmatched = max(matched[pos]['static_setup_count'], key=lambda move: float(ref[move]['sim_wp']))
        move = chosen(primary[pos]['setup_wide'])
        deep_examples.append({'pos': pos, 'setup_move': move, 'rank': int(ref[move]['global_static_rank']),
                              'best25_move': best25, 'best_matched_move': bestmatched,
                              'gain_vs_best25_pp': 100 * (float(ref[move]['sim_wp']) - float(ref[best25]['sim_wp'])),
                              'gain_vs_best_matched_pp': 100 * (float(ref[move]['sim_wp']) - float(ref[bestmatched]['sim_wp']))})
    out = {'protocol': protocol, 'completion': completion, 'audit': audits, 'primary': primary_result,
           'by_phase_exploratory': phases, 'exploratory': exploratory, 'deep_audit': deep_audit,
           'deep_selected_subset': deep_setup, 'deep_examples': deep_examples,
           'limitations': ['Position-level sim estimates; no played-game strength estimate.',
                           'Balanced phases differ from natural turn frequency.',
                           'Independent reference uses four-ply static rollouts and a win-probability model.',
                           'Deep subset is enriched for different choices and cannot estimate population average.',
                           'Secondary p-values are exploratory and uncorrected for multiple comparisons.']}
    save(D / 'summary.json', out)
    with (D / 'paired_outcomes.csv').open('w') as file:
        writer = csv.DictWriter(file, fieldnames=['pos', 'phase', 'game', 'left_move', 'right_move', 'wp_gain_pp'])
        writer.writeheader(); writer.writerows(row | {'phase': meta[row['pos']]['phase'], 'game': meta[row['pos']]['game']} for row in primary_result['per_position'])
    result_rows = []
    for label, result in [('Setup vs equal-count static (primary)', primary_result)] + [(key, value) for key, value in phases.items()] + [(key, value) for key, value in exploratory.items()]:
        ci = result['bootstrap95_pp']
        result_rows.append(f'<tr><td>{html.escape(label)}</td><td>{result["positions"]}</td><td>{result["mean_wp_gain_pp"]:+.5f}</td><td>{ci[0]:+.5f} to {ci[1]:+.5f}</td><td>{result["t_test_two_sided_p"]:.5g}</td><td>{result["different_choices"]}</td></tr>')
    verdict = 'Positive and statistically significant on the prespecified primary test.' if primary_result['statistically_significant_primary'] and primary_result['mean_wp_gain_pp'] > 0 else 'The prespecified primary test did not establish a positive setup advantage.'
    if primary_result['statistically_significant_primary'] and primary_result['mean_wp_gain_pp'] < 0:
        verdict = 'Setup performs worse than equal-count static on the prespecified primary test.'
    if not completion['full_confirmation_sample']:
        verdict = 'Deadline-limited sample: the planned 1,200-position confirmation was not completed.'
    report = '<!doctype html><html><meta charset="utf-8"><title>MAGPIE fresh confirmation</title><style>body{font:15px system-ui;color:#263b31;max-width:1150px;margin:40px auto;padding:0 20px}table{border-collapse:collapse;width:100%}td,th{padding:9px;text-align:left;border-bottom:1px solid #ddd}pre{background:#f3f5ef;padding:16px;white-space:pre-wrap}</style><h1>Fresh independent-position confirmation</h1>'
    report += f'<p><b>{verdict}</b> Mean setup gain versus equal-count static: {primary_result["mean_wp_gain_pp"]:+.5f} percentage points; p={primary_result["t_test_two_sided_p"]:.5g}. Paired positions: {len(positions)} of 1,200.</p>'
    report += '<p>Each position comes from a separate fresh CSW24 game. The planned sample includes 400 early, 400 middle, and 400 late positions. All selection sims receive 15 seconds, with no-PAT static rollouts. Setup and its static control have exactly equal candidate counts. Selected plays are evaluated in the same independent 60-second four-ply reference. Zero-change positions are included. This measures candidate-selection value under this sim model.</p><p><a href="candidate-pools-debug.html">Explore full candidate lists, sortable sim results, and pass-relative adjustments</a> · <a href="PROTOCOL.json">Frozen protocol</a> · <a href="paired_outcomes.csv">Paired outcomes</a></p><table><tr><th>Comparison</th><th>N</th><th>Mean win gain (pp)</th><th>95% position bootstrap</th><th>Two-sided paired t p</th><th>Different choices</th></tr>' + ''.join(result_rows) + '</table>'
    report += '<p>' + ' '.join(out['limitations']) + '</p><h2>Deep validation</h2><pre>' + html.escape(json.dumps({'audit': deep_audit, 'examples': deep_examples}, indent=2)) + '</pre><h2>Integrity checks</h2><pre>' + html.escape(json.dumps(audits, indent=2)) + '</pre></html>'
    (D / 'REPORT.html').write_text(report)
    save(D / 'result_hashes.json', {path.name: hashlib.sha256(read_bytes_retry(path)).hexdigest()
                                  for path in list(D.glob('*.csv')) + [D / 'summary.json', D / 'REPORT.html']})
    print(json.dumps({key: value for key, value in primary_result.items() if key != 'per_position'}))


if __name__ == '__main__':
    main()
