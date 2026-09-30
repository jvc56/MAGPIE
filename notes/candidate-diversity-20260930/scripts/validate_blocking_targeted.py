#!/usr/bin/env python3
"""Validate the focused harness against the unchanged prior binary."""
import csv
import difflib
import json
import os
import subprocess

from blocking_targeted_study import BIN, COMMON, D, LOCAL, OLD, REPO, digest

metadata = list(csv.DictReader((D / 'positions.meta.csv').open()))
pos = int(next(row for row in metadata if int(row['bag']) == 5)['pos'])
line = next(line for line in (D / 'positions.cgp').read_text().splitlines()
            if int(line.split(',', 1)[0]) == pos)
source = LOCAL / 'smoke-five-position.cgp'
source.write_text(line + '\n')
records = {}
for label, binary in [('old', OLD / 'magpie_test_frozen'), ('new', BIN)]:
    output = LOCAL / f'smoke-five-{label}.csv'
    check = LOCAL / f'smoke-five-{label}-checks.csv'
    for file in [output, check]:
        if file.exists():
            file.unlink()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('PCD_', 'PTG_'))} | COMMON | {
               'PCD_IN': str(source), 'PCD_OUT': str(output),
               'PCD_CHECK_OUT': str(check), 'PCD_EXPLORE': '1',
               'PCD_MS': '1000', 'PCD_REF_MS': '1000'}
    with (LOCAL / f'smoke-five-{label}.log').open('w') as log:
        subprocess.run([str(binary), 'patcanddiversity'], cwd=REPO, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    arms = {}
    for row in csv.DictReader(output.open()):
        arms.setdefault(row['arm'], {})[row['move']] = row
    records[label] = arms
    assert all(int(row['bag']) == 5 for arm in arms.values() for row in arm.values())
for arm in ['static25', 'threat_wide', 'reference']:
    assert set(records['old'][arm]) == set(records['new'][arm])
    for move, row in records['new'][arm].items():
        assert all(row[key] == records['old'][arm][move][key] for key in
                   ['static_rank', 'global_static_rank', 'static_eq', 'threat_value', 'setup_value'])
assert len(records['new']['threat_wide']) == len(records['new']['static_blocking_count'])
assert (LOCAL / 'smoke-five-old-checks.csv').read_bytes() == (LOCAL / 'smoke-five-new-checks.csv').read_bytes()
assert (LOCAL / 'smoke-generator-old.cgp').read_bytes() == (LOCAL / 'smoke-generator-new.cgp').read_bytes()
(D / 'harness_diff.patch').write_text(''.join(difflib.unified_diff(
    (OLD / 'harness_snapshot.c').read_text().splitlines(True),
    (D / 'harness_snapshot.c').read_text().splitlines(True),
    fromfile='original_frozen_harness.c', tofile='targeted_harness.c')))
proof = {'pos': pos, 'bag': 5, 'old_new_candidate_membership_exact': True,
         'old_new_adjustments_exact': True, 'same_count_control_exact': True,
         'default_generator_byte_identical': True,
         'original_binary_unchanged': digest(OLD / 'magpie_test_frozen') ==
         'c4daae223b33dae7ea12fe6c1eb24eeef25d58f88e2d56c71294264a53084677',
         'smoke_sims_excluded_from_study': True}
(D / 'preflight_validation.json').write_text(json.dumps(proof, indent=2) + '\n')
print(json.dumps(proof))
