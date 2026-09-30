#!/usr/bin/env python3
"""Build an offline candidate-pool explorer from the saved research CSVs."""
import csv
import json
import re
import os
import io
import time
from collections import Counter
from pathlib import Path

PASS_SOURCE = Path('/Users/olaugh/sources/magpie-pat-postx/postx/threatgames/pass_relative_20260929')
DEFAULT_SOURCE = PASS_SOURCE if (PASS_SOURCE / 'complete.json').exists() else PASS_SOURCE.parent / 'extended_2h40'
SOURCE = Path(os.environ.get('CANDIDATE_DEBUG_SOURCE', str(DEFAULT_SOURCE)))
OUTPUT = Path(os.environ.get('CANDIDATE_DEBUG_OUTPUT', str(Path(__file__).parent / 'candidate-pools-debug.html')))
DATA_ROOT = Path(os.environ.get('CANDIDATE_DATA_ROOT', str(SOURCE.parents[2] / 'data')))
IS_PASS_RELATIVE = SOURCE.name.startswith(('pass_relative', 'overnight-confirm'))

def read_text_retry(path):
    for attempt in range(3):
        try:
            return path.read_text()
        except OSError:
            if attempt == 2:
                raise
            time.sleep(1)

def rows(path):
    return list(csv.DictReader(io.StringIO(read_text_retry(path))))

def board(cgp):
    result = []
    for encoded in cgp.split()[0].split('/'):
        line = []
        for token in re.findall(r'\d+|.', encoded):
            line.extend([''] * int(token) if token.isdigit() else [token])
        assert len(line) == 15
        result.append(line)
    assert len(result) == 15
    return result

def dataset(directory, name, files, position_files, arms, allowed_ids=None):
    check_records = {}
    for check_file in list(directory.glob('w[0-7]_checks.csv'))+([directory/'r0_checks.csv'] if (directory/'r0_checks.csv').exists() else []):
        for record in rows(check_file):
            check_records[int(record['pos']), record['move']] = record
    positions = {}
    metadata = {int(row['pos']): row for row in rows(directory / 'positions.meta.csv')}
    for filename in position_files:
        for line in read_text_retry(directory / filename).splitlines():
            identifier, cgp = line.split(',', 1)
            identifier = int(identifier)
            if allowed_ids is not None and identifier not in allowed_ids:
                continue
            fields = cgp.split()
            positions[identifier] = dict(id=identifier, cgp=cgp, board=board(cgp), racks=fields[1].split('/'), scores=list(map(int, fields[2].split('/'))), onturn=0, scoreless=int(fields[3]), meta=metadata.get(identifier, {}), candidates={}, pools={})
    for row in rows(directory / 'candidate_features.csv'):
        if int(row['pos']) not in positions:
            continue
        position = positions[int(row['pos'])]
        position['candidates'][row['move']] = dict(move=row['move'], index=int(row['pool_index']), rank=int(row['global_static_rank']), score=float(row['score']), eq=float(row['static_eq']), leave=row['leave'], tiles=int(row['tiles_played']), type=int(row['type']), results={})
    records = {}
    for filename in files:
        for row in rows(directory / filename):
            records[int(row['pos']), row['arm'], row['move']] = row
    if (directory / 'r0.csv').exists():
        for row in rows(directory / 'r0.csv'):
            records[int(row['pos']), row['arm'], row['move']] = row
    for (identifier, arm, move), row in records.items():
        if identifier not in positions or arm not in arms + ['reference']:
            continue
        position = positions[identifier]
        candidate = position['candidates'][move]
        assert candidate['rank'] == int(row['global_static_rank'])
        assert abs(candidate['eq'] - float(row['static_eq'])) < 0.0000001
        if arm not in ['static_setup_count', 'static_combined_count']:
            candidate['threat'] = float(row['threat_value'])
            candidate['setup'] = float(row['setup_value'])
        candidate['results'][arm] = [float(row[key]) for key in ['sim_wp', 'sim_wp_sem', 'sim_eq', 'sim_eq_sem']] + [int(row['chosen'])]
        position['pools'][arm] = dict(count=int(row['candidates']), seconds=round(float(row['wall_ms']) / 1000, 2))
        position['bag'] = int(row['bag'])
    # Supplementary deeper references are kept separate from the main reference.
    for identifier, position in positions.items():
        deepfile = directory / (f'new{identifier}_all25_p16.csv' if name == 'primary' else f'validate{identifier}_p16.csv')
        if deepfile.exists():
            for row in rows(deepfile):
                if row['move'] in position['candidates']:
                    candidate = position['candidates'][row['move']]
                    candidate['results']['deep16'] = [float(row[key]) for key in ['sim_wp', 'sim_wp_sem', 'sim_eq', 'sim_eq_sem']] + [int(row['chosen'])]
                    position['pools']['deep16'] = dict(count=int(row['candidates']), seconds=round(float(row['wall_ms']) / 1000, 2))
        for candidate in position['candidates'].values():
            record = check_records.get((identifier, candidate['move']))
            if record:
                candidate['check'] = {key: float(record[key]) for key in ['pass_reply_mean', 'candidate_reply_mean', 'blocking_delta', 'blocking_adjustment', 'pass_followup_mean', 'candidate_followup_mean', 'setup_delta', 'setup_adjustment']}
                assert abs(candidate['check']['blocking_adjustment'] - 2 * (candidate['threat'] - candidate['eq'])) < 0.000002
                assert abs(candidate['check']['setup_adjustment'] - 3 * (candidate['setup'] - candidate['eq'])) < 0.000002
            if IS_PASS_RELATIVE:
                assert record is not None, (identifier, candidate['move'])
        base = arms[0]
        raw_sources = {}
        if name == 'primary':
            eligible = [candidate for candidate in position['candidates'].values() if candidate['eq'] >= max(item['eq'] for item in position['candidates'].values()) - 25]
            for component in ['threat', 'setup']:
                full_ranking = sorted(eligible, key=lambda candidate: (-(candidate[component] - candidate['eq']), candidate['index']))
                ranked = full_ranking[:5]
                for candidate in ranked:
                    raw_sources.setdefault(candidate['move'], []).append('signals_' + component)
                if len(full_ranking) > 5 and abs((full_ranking[4][component] - full_ranking[4]['eq']) - (full_ranking[5][component] - full_ranking[5]['eq'])) < 0.000003:
                    raw_sources['ambiguous_boundary'] = []
            # Ensure the reconstruction from exported values exactly reproduces
            # the recorded union before exposing the finer provenance labels.
            recorded = {candidate['move'] for candidate in position['candidates'].values() if 'signals' in candidate['results'] and base not in candidate['results']}
            reconstructed = {move for move in raw_sources if move in position['candidates'] and base not in position['candidates'][move]['results']}
            if recorded != reconstructed or 'ambiguous_boundary' in raw_sources:
                # Rounded CSV scores can hide tie order. Keep the recorded
                # mixed-source provenance rather than assert a finer source.
                raw_sources = {}
        for candidate in position['candidates'].values():
            candidate['sources'] = [] if base in candidate['results'] else [arm for arm in arms[1:] if arm not in ['combined', 'tile_exchange'] and arm in candidate['results']]
            if 'signals' in candidate['sources'] and raw_sources:
                candidate['sources'].remove('signals')
                candidate['sources'].extend(raw_sources[candidate['move']])
            if name == 'quota' and base not in candidate['results'] and 'tile_exchange' in candidate['results'] and 'tile_quota' not in candidate['results']:
                candidate['sources'].append('exchange')
            # Check all placements against the board, including played-through tiles.
            if candidate['type'] == 1:
                coordinate, word = candidate['move'].split()
                vertical = coordinate[0].isalpha()
                column = ord(re.search('[a-o]', coordinate).group()) - ord('a')
                rowindex = int(re.search(r'\d+', coordinate).group()) - 1
                added = 0
                for offset, letter in enumerate(word):
                    row = rowindex + (offset if vertical else 0)
                    col = column + (0 if vertical else offset)
                    assert 0 <= row < 15 and 0 <= col < 15, candidate['move']
                    existing = position['board'][row][col]
                    assert not existing or existing == letter, (identifier, candidate['move'], existing, letter)
                    added += not bool(existing)
                assert added == candidate['tiles'], (identifier, candidate['move'], added, candidate['tiles'])
        for arm in arms + ['reference']:
            assert sum(arm in candidate['results'] for candidate in position['candidates'].values()) == position['pools'][arm]['count']
            assert sum(candidate['results'].get(arm, [0]*5)[4] for candidate in position['candidates'].values()) == 1
        distribution = Counter({row[0]: int(row[2]) for row in csv.reader((DATA_ROOT / 'letterdistributions/english.csv').open())})
        for line in position['board']:
            for letter in line:
                if letter:
                    distribution['?' if letter.islower() else letter] -= 1
        distribution.subtract(position['racks'][position['onturn']])
        assert all(count >= 0 for count in distribution.values())
        assert sum(distribution.values()) == position['bag'] + len(position['racks'][1-position['onturn']])
        position['unseen'] = dict(distribution)
        position['candidates'] = sorted(position['candidates'].values(), key=lambda candidate: candidate['rank'])
    return dict(name=name, arms=arms, positions=list(positions.values()))


def main():
    data = dict(datasets=[
        dataset(SOURCE, 'primary', [f'w{index}.csv' for index in range(8)], ['positions.cgp', 'openings.cgp'], ['static25', 'threat_wide', 'setup_wide', 'signals', 'exchange', 'combined']),
        dataset(SOURCE / 'tile_followup', 'quota', [file.name for file in sorted((SOURCE / 'tile_followup').glob('w[0-3].csv'))], ['positions.cgp'], ['static25_fulltime', 'tile_quota', 'tile_exchange']),
    ], premiums=[list(line) for line in (DATA_ROOT / 'layouts/standard15.txt').read_text().splitlines()[1:]], tileScores={row[0]: int(row[3]) for row in csv.reader((DATA_ROOT / 'letterdistributions/english.csv').open())})
    assert len(data['premiums']) == 15 and all(len(row) == 15 for row in data['premiums'])
    template = (Path(__file__).parent / 'candidate_debug_template.html').read_text()
    if IS_PASS_RELATIVE:
        template = template.replace('Legacy setup + blocking', 'Pass-relative setup + blocking').replace('Legacy quotas', 'Pass-relative quotas')
        template = template.replace('The pass-relative rerun is in progress; this is the legacy data.', 'Pass-relative checks; every distinct pool receives a full 15-second sim.')
        template = template.replace('setup = 3 × (exported setup value − static equity). A smaller blocking penalty ranks better; a larger setup bonus ranks better.', 'setup = 3 × (exported setup value − static equity). Positive blocking means reduced opponent scoring versus a pass; positive setup means improved next-turn scoring versus the matched pass branch.')
        template = template.replace('Blocking ranks plays by static equity minus 1.4 times the mean best opponent placement reply score. Setup ranks plays by static equity plus 0.75 times the setup component.', 'Blocking adds 1.4 times the mean reduction in the opponent’s best placement score versus passing. Setup adds 0.75 times the change in our best next-turn placement score after the opponent reply, versus passing followed by the opponent reply. Both branches use the same sampled opponent rack, and setup uses the same candidate leave/refill in both branches. Refills exclude the sampled opponent rack. The main checks average 64 sampled racks. If the opponent reply ends the game, our next-turn opportunity is zero.')
        template = template.replace('Primary pool sims have a 15-second maximum and may stop early;', 'Primary pool sims run for the full 15-second budget;')
        template = template.replace('Repaired references replace seven original runs.', 'These references were rerun for this study.')
    OUTPUT.write_text(template.replace('/*DATA*/null', json.dumps(data, separators=(',', ':'))))
    position_count = sum(len(item['positions']) for item in data['datasets'])
    print(f'Built {OUTPUT} ({OUTPUT.stat().st_size:,} bytes); verified {position_count} positions, all pool counts, choices, and tile overlays.')


if __name__ == '__main__':
    main()
