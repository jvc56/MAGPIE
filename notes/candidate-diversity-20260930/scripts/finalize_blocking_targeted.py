#!/usr/bin/env python3
"""Apply reviewed reporting fixes and audit finished targeted-study artifacts."""
import fcntl
import json
import math
import shutil
import time

import blocking_targeted_study as study


def main():
    directory = study.D
    lock = (directory / 'coordinator.lock').open('a')
    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    assert (directory / 'data_complete.json').exists()
    complete = json.loads(study.read_text_retry(directory / 'complete.json'))
    if complete.get('reviewed_postprocessing_complete'):
        print('Reviewed postprocessing already complete.')
        return
    assert not (directory / 'failure.json').exists()
    archive = directory / 'postprocessing-before-review'
    archive.mkdir(exist_ok=True)
    for name in ['summary.json', 'paired_outcomes.csv', 'REPORT.html',
                 'candidate-pools-debug.html', 'result_hashes.json',
                 'deliverable_hashes.json', 'complete.json']:
        source = directory / name
        target = archive / name
        if source.exists() and not target.exists():
            shutil.copy2(source, target)
    study.analyze()
    study.page()
    summary = json.loads(study.read_text_retry(directory / 'summary.json'))
    paired = list(study.csv_rows(directory / 'paired_outcomes.csv'))
    assert len(paired) == study.N
    assert len({int(row['pos']) for row in paired}) == study.N
    manifest = json.loads(study.read_text_retry(directory / 'deep_manifest.json'))
    deep_ids = {row['pos'] for row in summary['deep']['per_position']}
    assert deep_ids == {row['pos'] for row in manifest['targets']}
    for identifier in deep_ids:
        for row in study.csv_rows(directory / f'new{identifier}_all25_p16.csv'):
            assert 0 <= float(row['sim_wp']) <= 1
            assert math.isfinite(float(row['sim_wp_sem'])) and float(row['sim_wp_sem']) >= 0
            assert int(row['iterations']) > 0 and float(row['wall_ms']) >= 599500
    for manifest_name in ['input_hashes.json', 'result_hashes.json', 'deliverable_hashes.json']:
        hashes = json.loads(study.read_text_retry(directory / manifest_name))
        assert all(study.digest(directory / name) == value for name, value in hashes.items())
    before = json.loads(study.read_text_retry(archive / 'summary.json'))
    for key in ['positions', 'mean_wp_gain_pp', 'position_se_pp', 'bootstrap95_pp',
                'different_choices', 'positive', 'negative', 'zero']:
        assert before['primary'][key] == summary['primary'][key]
    if before['primary']['t_test_two_sided_p'] is not None:
        assert before['primary']['t_test_two_sided_p'] == summary['primary']['t_test_two_sided_p']
    validation = {'finished_epoch': time.time(), 'positions': study.N,
                  'deep_positions': len(deep_ids), 'all_hashes_valid': True,
                  'primary_effect_interval_and_test_unchanged_by_reporting_review': True,
                  'mc_uncertainty_aggregated_as_standard_error_of_mean': True,
                  'browser_verified': False}
    study.save(directory / 'final_validation.json', validation)
    study.save(directory / 'complete.json', complete | {
        'reviewed_postprocessing_complete': True, 'reviewed_epoch': time.time()})
    print(json.dumps(validation), flush=True)


if __name__ == '__main__':
    main()
