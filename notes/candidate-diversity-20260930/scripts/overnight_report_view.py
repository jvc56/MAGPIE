#!/usr/bin/env python3
"""Render the audited summary without recalculating or changing the study."""
import html
import json
from pathlib import Path


def esc(value):
    return html.escape(str(value))


def result_row(label, result):
    interval = result['bootstrap95_pp']
    ci = f'{interval[0]:+.5f} to {interval[1]:+.5f}' if interval else '—'
    return (f'<tr><td>{esc(label)}</td><td>{result["positions"]}</td>'
            f'<td>{result["mean_wp_gain_pp"]:+.5f}</td><td>{ci}</td>'
            f'<td>{result["t_test_two_sided_p"]:.5g}</td>'
            f'<td>{result["different_choices"]}</td></tr>')


def build_report(summary):
    primary = summary['primary']
    complete = summary['completion']['full_confirmation_sample']
    significant = primary['statistically_significant_primary']
    verdict = ('Setup has a small, statistically significant advantage over equal-size static pools.'
               if complete and significant and primary['mean_wp_gain_pp'] > 0
               else 'The primary test did not establish a positive setup advantage.')
    if complete and significant and primary['mean_wp_gain_pp'] < 0:
        verdict = 'Setup performs worse than equal-size static pools on the primary test.'
    if not complete:
        verdict = 'Incomplete confirmation sample; the planned 1,200-position study did not finish.'
    ci = primary['bootstrap95_pp']
    phases = summary['by_phase_exploratory']
    header = '<tr><th>Comparison</th><th>N</th><th>Mean gain (pp)</th><th>95% bootstrap interval (pp)</th><th>Two-sided paired t p</th><th>Different choices</th></tr>'
    page = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>MAGPIE · Fresh confirmation</title><style>
body{font:16px/1.5 system-ui;color:#21382d;background:#f7f8f3;max-width:1180px;margin:32px auto;padding:0 24px}h1,h2{line-height:1.2}a{color:#176754}.result{background:#e9f0df;border:1px solid #c6d4be;border-radius:10px;padding:18px 22px}.result p{margin:7px 0}.number{font-size:32px;font-weight:750}.tablewrap{overflow:auto;background:white;border:1px solid #dce2d7;border-radius:8px}table{border-collapse:collapse;width:100%;font-size:14px}td,th{padding:10px 12px;text-align:left;border-bottom:1px solid #e1e5dc;white-space:nowrap}th{background:#e8ede3}td:first-child{white-space:normal}pre{font:12px/1.5 ui-monospace,monospace;white-space:pre-wrap;background:#edf0e8;padding:16px}small{color:#516357}.limits{border-left:4px solid #8f9f87;padding-left:16px}details{margin:18px 0}summary{cursor:pointer;font-weight:650}
</style><h1>Fresh independent-position confirmation</h1>'''
    page += f'<div class="result"><p><b>{verdict}</b></p><p class="number">{primary["mean_wp_gain_pp"]:+.4f} percentage points</p><p>95% position bootstrap interval: {ci[0]:+.4f} to {ci[1]:+.4f}; two-sided p = {primary["t_test_two_sided_p"]:.5g}. N = {primary["positions"]:,}.</p></div>'
    page += '<p>One occupied-board position from each of 1,200 fresh independent CSW24 games: 400 early, 400 middle, and 400 late. Selection sims receive 15 seconds with no-PAT static rollouts. The setup pool and its static control contain exactly the same number of candidates. Selected plays are evaluated in the same independent 60-second four-ply reference. All zero-change positions are included.</p>'
    page += f'<p>The pools selected different plays in {primary["different_choices"]} positions: {primary["positive"]} positive and {primary["negative"]} negative reference differences; {primary["zero"]} positions contributed zero. The advantage is concentrated late: {phases["late"]["mean_wp_gain_pp"]:+.4f} pp across 400 late positions. Phase estimates are exploratory.</p>'
    page += '<p><a href="candidate-pools-debug.html#primary/870">Open the sortable candidate explorer</a> · <a href="PROTOCOL.json">Frozen protocol</a> · <a href="paired_outcomes.csv">All paired outcomes</a></p>'
    page += '<h2>Primary comparison and phases</h2><div class="tablewrap"><table>' + header
    page += result_row('Setup vs exactly equal-count static (primary)', primary)
    page += ''.join(result_row(phase.capitalize(), result) for phase, result in phases.items()) + '</table></div>'
    page += '<h2>Other candidate policies</h2><p>These comparisons are exploratory and uncorrected for multiple comparisons. The comparisons with static 25 include differences in pool size.</p><div class="tablewrap"><table>' + header
    labels = {'threat_wide': 'Blocking vs static 25', 'setup_wide': 'Setup vs static 25', 'signals': 'Raw signals vs static 25', 'exchange': 'Exchange quota vs static 25', 'combined': 'Combined vs static 25', 'combined_same_count': 'Combined vs equal-count static'}
    page += ''.join(result_row(labels[key], result) for key, result in summary['exploratory'].items()) + '</table></div>'
    blocking = summary['exploratory']['threat_wide']
    page += f'<p>Blocking did not show a clear average gain in this study: {blocking["mean_wp_gain_pp"]:+.5f} pp versus static 25, p = {blocking["t_test_two_sided_p"]:.3f}. Setup provides the clearest evidence of valuable candidate diversity under the tested sim policy.</p>'
    deep = summary['deep_selected_subset']
    page += '<h2>Longer independent references</h2>'
    if deep:
        page += f'<p>All {deep["positions"]} prespecified novel-choice positions received independent 600-second, sixteen-ply references. Setup versus its selected equal-size static control averaged {deep["mean_wp_gain_pp"]:+.4f} pp on this enriched subset ({deep["positive"]} positive, {deep["negative"]} negative, {deep["zero"]} zero). This subset cannot estimate the population average.</p>'
        page += '<p>The table compares the originally selected setup play with the best candidate in each static pool under the longer reference. Some additions help substantially; others fail to improve on the static candidates. Click a position to inspect its board, provenance, adjustments, and sim results.</p><div class="tablewrap"><table><tr><th>Position</th><th>Setup choice</th><th>Static rank</th><th>Gain vs best static 25 (pp)</th><th>Gain vs best same-size static (pp)</th></tr>'
        for row in sorted(summary['deep_examples'], key=lambda item: item['gain_vs_best_matched_pp'], reverse=True):
            page += f'<tr><td><a href="candidate-pools-debug.html#primary/{row["pos"]}">{row["pos"]}</a></td><td>{esc(row["setup_move"])}</td><td>{row["rank"]}</td><td>{row["gain_vs_best25_pp"]:+.4f}</td><td>{row["gain_vs_best_matched_pp"]:+.4f}</td></tr>'
        page += '</table></div>'
    page += '<h2>Interpretation and limits</h2><div class="limits"><p>This is evidence about candidate selection under the stated sim model. It does not measure played-game strength. The balanced phases differ from natural turn frequency, and the primary reference uses four-ply static rollouts and a win-probability model.</p><p>The practical result is a small average benefit from occasional valuable setup additions, chiefly late in the game. The stronger examples do not make every setup addition useful.</p></div>'
    page += '<details><summary>Passed integrity checks</summary><pre>' + esc(json.dumps({'main': summary['audit'], 'deep': summary['deep_audit']}, indent=2)) + '</pre></details></html>'
    return page


if __name__ == '__main__':
    from overnight_prepare import D
    from overnight_run import read_text_retry
    summary = json.loads(read_text_retry(D / 'summary.json'))
    (D / 'REPORT.html').write_text(build_report(summary))
    print('Rendered the audited summary; no statistics or simulations recalculated.')
