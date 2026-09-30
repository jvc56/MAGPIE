#!/usr/bin/env python3
"""Prepare the frozen, independent-game confirmation sample."""
import concurrent.futures
import csv
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent
D = ROOT / 'overnight-confirm-20260930'
REPO = Path('/Users/olaugh/sources/magpie-pat-pass-relative')
PREVIOUS = Path('/Users/olaugh/sources/magpie-pat-postx/postx/threatgames/pass_relative_20260929')
BIN = D / 'magpie_test_frozen'
GEN_SEED = 202609300700
SAMPLE_SEED = 202609300701
SIM_SEED = 202609300702
DEADLINE = 1790776800  # 2026-09-30 14:00 UTC, 7am Pacific


def save(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def phase(bag):
    return 'early' if bag >= 60 else 'middle' if bag >= 30 else 'late'


def generate(batch):
    target = D / 'source_games' / f'batch{batch:02}.cgp'
    if target.exists() and target.with_suffix('.done.json').exists():
        return target
    env = os.environ | {'PCD_GENERATE_OUT': str(target), 'PCD_GENERATE_GAMES': '100',
                        'PCD_GENERATE_SEED': str(GEN_SEED + batch * 10000)}
    with target.with_suffix('.log').open('w') as log:
        subprocess.run([str(BIN), 'patcanddiversity'], cwd=REPO, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    save(target.with_suffix('.done.json'), {'batch': batch, 'seed': GEN_SEED + batch * 10000,
                                         'finished_epoch': time.time()})
    return target


def features(input_path, prefix):
    workers = []
    for worker in range(4):
        out = D / f'{prefix}{worker}.csv'
        env = os.environ | {'PCD_IN': str(input_path), 'PCD_OUT': str(D / f'{prefix}{worker}_unused.csv'),
                            'PCD_FEATURE_OUT': str(out), 'PCD_FEATURE_ONLY': '1',
                            'PCD_ADAPTIVE': '1', 'PCD_POOL': '120', 'PCD_EXCHANGE_QUOTA': '5',
                            'PCD_EXCHANGE_MARGIN': '35', 'PCD_WORKER': str(worker), 'PCD_NUM_WORKERS': '4'}
        log = (D / f'{prefix}{worker}.log').open('w')
        workers.append(subprocess.Popen([str(BIN), 'patcanddiversity'], cwd=REPO, env=env,
                                        stdout=log, stderr=subprocess.STDOUT))
        log.close()
    assert all(process.wait() == 0 for process in workers)
    records = {}
    for worker in range(4):
        for row in csv.DictReader((D / f'{prefix}{worker}.csv').open()):
            records.setdefault(int(row['pos']), []).append(row)
    return records


def main():
    D.mkdir(exist_ok=True)
    (D / 'source_games').mkdir(exist_ok=True)
    if (D / 'prepared.json').exists():
        print('Already prepared. Frozen inputs retained.')
        return
    if not BIN.exists():
        shutil.copy2(PREVIOUS / 'magpie_test_pass_relative', BIN)
        shutil.copy2(PREVIOUS / 'harness_snapshot.c', D / 'harness_snapshot.c')
    protocol = {
        'authorized': 'User chose Extend overnight, through 7am September 30 Pacific, 1200 independent positions.',
        'deadline_epoch': DEADLINE, 'deadline_local': '2026-09-30 07:00 America/Los_Angeles',
        'locked_epoch': time.time(), 'planned_positions': 1200, 'one_position_per_independent_game': True,
        'population': 'CSW24 no-PAT static autoplay; occupied board; bag >=7; at least 50 static candidates.',
        'strata': {'early_bag_60_plus': 400, 'middle_bag_30_59': 400, 'late_bag_7_29': 400},
        'sampling': 'Game index assigns stratum in rotation. Uniform randomized order of positions in that stratum; first eligible position. No sim outcomes used. Fresh reserve games replace missing strata.',
        'primary': 'setup_wide versus static_setup_count, exactly equal candidate counts; paired selected-play win probability difference in an independent 60-second 4-ply reference.',
        'analysis': 'All 1200 positions including unchanged choices. Two-sided paired t test alpha 0.05, 95% position bootstrap, phase estimates exploratory. Report incomplete if deadline prevents full sample.',
        'interpretation': 'Balanced position-level sim-policy study, not natural turn-weighting or full played-game strength.',
        'selection_ms': 15000, 'selection_cutoff': 'disabled; identical pools reuse the same completed full-budget result',
        'reference_ms': 60000, 'plies': 4, 'rollout': 'no-PAT static', 'rack_samples': 64,
        'blocking': '1.4 * mean(opponent best placement score after pass minus after candidate), paired racks',
        'setup': '0.75 * mean(our best next-turn placement after candidate/opponent reply minus after pass/opponent reply); same candidate leave/refill; opponent terminal reply gives next-turn score zero',
        'conditioned_refills': 'Exclude sampled opponent rack; same refill in both setup branches',
        'pool': 'Top 120 static plus up to 5 exchange candidates within 35 points of the top play',
        'admission': 'Static top25 union adjusted top25 for blocking/setup; raw top5 per signal within25; combined union; adaptive counts',
        'exploratory': ['blocking/exchange/raw/combined vs static25', 'combined vs same count where enough static candidates', 'up to80 novel-choice positions, hash-selected without reference gains, 600s16ply validation'],
        'deep_selection': 'Any primary or matched policy selects outside static25. Up to80 positions by SHA256(seed,pos), outcome blind. Validate all matched static-prefix candidates and all selected plays.',
        'deadline_policy': 'Main waits for prior rerun completion, stops starting positions 2h before deadline. Matched stops starting positions 15min before deadline. Only complete-position intersection analyzed if incomplete. Deep waves start only if 11min remain.',
        'seeds': {'games': GEN_SEED, 'sample': SAMPLE_SEED, 'sim': SIM_SEED, 'deep': 202609300703},
        'binary_sha256': hashlib.sha256(BIN.read_bytes()).hexdigest(),
        'harness_sha256': hashlib.sha256((D / 'harness_snapshot.c').read_bytes()).hexdigest(),
        'original_rerun': str(PREVIOUS), 'weights_locked_before_outcomes': True,
    }
    if not (D / 'PROTOCOL.json').exists():
        save(D / 'PROTOCOL.json', protocol)
    rng = random.Random(SAMPLE_SEED)
    selections = []
    alternatives = {}
    excluded = []
    batches = 12
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        list(executor.map(generate, range(batches)))
    source_game = 0
    used_games = set()
    for batch in range(batches):
        target = D / 'source_games' / f'batch{batch:02}.cgp'
        cgps = {int(line.split(',', 1)[0]): line.split(',', 1)[1] for line in target.read_text().splitlines()}
        grouped = {}
        for row in csv.DictReader(Path(str(target) + '.meta.csv').open()):
            grouped.setdefault(int(row['game']), []).append(row)
        for game in range(100):
            desired = ['early', 'middle', 'late'][source_game % 3]
            eligible = [row for row in grouped[game] if phase(int(row['bag'])) == desired and int(row['turn']) > 0]
            rng.shuffle(eligible)
            if not eligible:
                excluded.append({'batch': batch, 'game': game, 'reason': 'missing requested occupied-board stratum'})
                continue
            pos = len(selections) + 1
            def item(row):
                return {'pos': pos, 'game': batch * 100 + game, 'turn': int(row['turn']),
                        'bag': int(row['bag']), 'phase': desired, 'batch': batch,
                        'source_pos': int(row['pos']), 'source_seed': GEN_SEED + batch * 10000 + game,
                        'cgp': cgps[int(row['pos'])]}
            choices = [item(row) for row in eligible]
            selections.append(choices[0]); alternatives[pos] = choices[1:]
            used_games.add((batch, game)); source_game += 1
    assert len(selections) == 1200, 'Missing phase: prepare explicit reserve-game replacement before proceeding.'
    def write_positions(path):
        path.write_text(''.join(f"{row['pos']},{row['cgp']}\n" for row in selections))
    preflight = D / 'preflight.cgp'
    write_positions(preflight)
    all_features = features(preflight, 'preflight_features_')
    while True:
        bad = [row for row in selections if sum(int(f['pool_index']) <= 120 and int(f['global_static_rank']) == int(f['pool_index']) for f in all_features.get(row['pos'], [])) < 50]
        if not bad:
            break
        for row in bad:
            excluded.append({key: value for key, value in row.items() if key != 'cgp'} | {'reason': 'fewer than50 static candidates'})
            assert alternatives[row['pos']], 'No eligible replacement in game; reserve replacement needed.'
            replacement = alternatives[row['pos']].pop(0)
            selections[row['pos'] - 1] = replacement
        replacement_input = D / 'replacement.cgp'
        replacement_input.write_text(''.join(f"{row['pos']},{selections[row['pos']-1]['cgp']}\n" for row in bad))
        all_features.update(features(replacement_input, 'replacement_features_'))
    assert len({row['source_seed'] for row in selections}) == 1200
    assert all(sum(row['phase'] == target for row in selections) == 400 for target in ['early', 'middle', 'late'])
    rng.shuffle(selections)
    write_positions(D / 'positions.cgp')
    with (D / 'positions.meta.csv').open('w') as out:
        writer = csv.DictWriter(out, fieldnames=[key for key in selections[0] if key != 'cgp'])
        writer.writeheader(); writer.writerows({key: value for key, value in row.items() if key != 'cgp'} for row in selections)
    with (D / 'candidate_features.csv').open('w') as out:
        writer = csv.DictWriter(out, fieldnames=list(next(iter(all_features.values()))[0]))
        writer.writeheader()
        for pos in sorted(all_features):
            writer.writerows(all_features[pos])
    (D / 'openings.cgp').write_text('')
    save(D / 'sampling_exclusions.json', excluded)
    save(D / 'input_hashes.json', {str(path.relative_to(D)): hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in [D/'positions.cgp', D/'positions.meta.csv', D/'candidate_features.csv', D/'PROTOCOL.json']})
    save(D / 'prepared.json', {'finished_epoch': time.time(), 'positions': 1200, 'independent_games': 1200,
                             'strata': {'early': 400, 'middle': 400, 'late': 400}, 'exclusions': len(excluded)})
    print(json.dumps(json.loads((D / 'prepared.json').read_text())))


if __name__ == '__main__':
    main()
