#!/usr/bin/env python3
"""Fresh fixed-sample blocking study for the user's bag/score conditions."""
import concurrent.futures
import csv
import fcntl
import hashlib
import io
import json
import math
import os
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import sys
import time

from overnight_run import csv_rows, full, load, read_bytes_retry, read_text_retry, selected
from overnight_analyze import interval, t_two_sided
from overnight_prepare import save

ROOT = Path(__file__).resolve().parent
D = ROOT / 'blocking-ahead-40-80-20260930'
OLD = ROOT / 'overnight-confirm-20260930'
REPO = Path('/Users/olaugh/sources/magpie-pat-pass-relative')
LOCAL = Path('/private/tmp/magpie-blocking-ahead-40-80-20260930')
BIN = D / 'magpie_test_targeted'
N = 400
WORKERS = 8
GEN_SEED = 202610010000
SAMPLE_SEED = 202609301001
SIM_SEED = 202609301002
DEEP_SEED = 202609301003
ARMS = ['static25', 'threat_wide', 'static_blocking_count', 'reference']
COMMON = {'PCD_PASS_RELATIVE': '1', 'PCD_ADAPTIVE': '1', 'PCD_BLOCKING_ONLY': '1',
          'PCD_POOL': '120', 'PCD_EXCHANGE_QUOTA': '5', 'PCD_EXCHANGE_MARGIN': '35',
          'PCD_CONDITION_DRAWS': '1', 'PCD_MS': '15000', 'PCD_REF_MS': '60000',
          'PCD_PLIES': '4', 'PCD_SELECTION_NO_CUTOFF': '1', 'PCD_SEED': str(SIM_SEED)}

BLOCKING_FUNCTION = r'''
// Targeted blocking study: unchanged admission formula, plus a same-size
// static control. All identical candidate sets share their completed sim.
static void pcd_run_blocking(Game *game, WinPct *win_pcts,
                             const Move *const *cands, int count,
                             int core_count, const double *threat,
                             const double *setup, long pos, uint64_t seed,
                             SimCtx **sim_ctx, SimResults *results, FILE *out) {
  bool selected[4][PTG_MAX_K] = {{false}};
  double adjusted[PTG_MAX_K];
  pcd_fill_static(selected[0], core_count, 25);
  memcpy(selected[1], selected[0], sizeof(selected[1]));
  for (int candidate_idx = 0; candidate_idx < count; candidate_idx++) {
    const double equity = equity_to_double(move_get_equity(cands[candidate_idx]));
    adjusted[candidate_idx] = equity + 2.0 * (threat[candidate_idx] - equity);
  }
  ptg_top_indices(adjusted, count, 25, selected[1]);
  int blocking_count = 0;
  for (int candidate_idx = 0; candidate_idx < count; candidate_idx++) {
    blocking_count += selected[1][candidate_idx];
  }
  assert(blocking_count <= core_count);
  pcd_fill_static(selected[2], core_count, blocking_count);
  pcd_fill_static(selected[3], count, count);
  const char *names[] = {"static25", "threat_wide", "static_blocking_count"};
  SimResults *cache[3] = {NULL};
  for (int step = 0; step < 3; step++) {
    const int arm = (step + (int)(pos % 3)) % 3;
    SimResults *reuse = NULL;
    for (int previous = 0; previous < 3; previous++) {
      if (cache[previous] != NULL && memcmp(selected[arm], selected[previous],
                                          sizeof(selected[arm])) == 0) {
        reuse = cache[previous];
        break;
      }
    }
    pcd_sim_arm(game, win_pcts, cands, count, selected[arm], threat, setup, pos,
                names[arm], (double)ptg_env_long("PCD_MS", 15000) / 1000.0,
                false, seed, sim_ctx, results, reuse, out);
    cache[arm] = sim_results_duplicate(reuse != NULL ? reuse : results);
  }
  pcd_sim_arm(game, win_pcts, cands, count, selected[3], threat, setup, pos,
              "reference", (double)ptg_env_long("PCD_REF_MS", 60000) / 1000.0,
              true, seed ^ UINT64_C(0xd1b54a32d192ed03), sim_ctx, results,
              NULL, out);
  for (int arm = 0; arm < 3; arm++) {
    sim_results_destroy(cache[arm]);
  }
}

'''


def digest(path):
    return hashlib.sha256(read_bytes_retry(path)).hexdigest()


def snapshot(path):
    data = read_bytes_retry(path)
    LOCAL.mkdir(exist_ok=True)
    target = LOCAL / (hashlib.sha256(data).hexdigest() + '-' + path.name)
    if not target.exists():
        target.write_bytes(data)
    assert target.read_bytes() == data
    return target


def executable():
    # Cloud-file rehydration can invalidate macOS executable mappings. Execute
    # byte-identical frozen code from the local volume, preserving its signature.
    target = snapshot(BIN)
    target.chmod(0o755)
    assert digest(target) == digest(BIN)
    save(D / 'runtime_binary.json', {'path': str(target), 'sha256': digest(target),
                                     'byte_identical_to_frozen_binary': True})
    return target


def stage(name, **fields):
    record = {'stage': name, 'epoch': time.time(), 'driver_pid': os.getpid(), **fields}
    save(D / 'progress.json', record)
    print(json.dumps(record), flush=True)


def build():
    D.mkdir(exist_ok=True)
    LOCAL.mkdir(exist_ok=True)
    if (D / 'build.json').exists():
        record = json.loads(read_text_retry(D / 'build.json'))
        assert digest(BIN) == record['binary_sha256']
        return
    original = read_text_retry(OLD / 'harness_snapshot.c')
    assert original.count('if (bag >= 7) {') == 1
    revised = original.replace('if (bag >= 7) {',
                               'if (bag >= ptg_env_long("PCD_GENERATE_MIN_BAG", 7)) {')
    revised = revised.replace('static void pcd_run_matched(', BLOCKING_FUNCTION + 'static void pcd_run_matched(', 1)
    trigger = '    if (adaptive && ptg_env_long("PCD_EXPLORE", 0) != 0) {'
    branch = '''    if (adaptive && ptg_env_long("PCD_BLOCKING_ONLY", 0) != 0) {
      pcd_run_blocking(game, win_pcts, cands, count, static_core_count, threat,
                       setup, pos, seed, &sim_ctx, results, out);
      fprintf(stderr, "PCD_BLOCKING_DONE pos=%ld bag=%d pool=%d\\n", pos,
              bag_get_letters(game_get_bag(game)), count);
      fflush(stderr);
      continue;
    }
'''
    assert revised.count(trigger) == 1
    revised = revised.replace(trigger, branch + trigger)
    source = D / 'harness_snapshot.c'
    source.write_text(revised)
    obj = D / 'pat_threat_games_test.o'
    subprocess.run(['cc', '-O3', '-flto', '-march=native', '-Wall', '-Wno-trigraphs',
                    '-DBOARD_DIM=15', '-DRACK_SIZE=7', '-I', str(REPO / 'test'),
                    '-c', str(source), '-o', str(obj)], check=True)
    obj_root = REPO / 'obj/no_pgo_release-b15-r7'
    objects = sorted((obj_root / 'src').glob('**/*.o')) + sorted((obj_root / 'test').glob('*.o'))
    objects = [path for path in objects if path.name != 'pat_threat_games_test.o']
    assert len(objects) > 100
    subprocess.run(['cc', '-pthread', '-flto', *map(str, objects), str(obj), '-lm', '-o', str(BIN)], check=True)
    save(D / 'build.json', {'binary_sha256': digest(BIN), 'harness_sha256': digest(source),
                           'original_harness_sha256': digest(OLD / 'harness_snapshot.c'),
                           'linked_objects_sha256': {str(path): digest(path) for path in objects},
                           'changes': ['Configurable saved-position minimum bag, default unchanged at7',
                                       'Three selection arms with equal-size blocking control; original scoring and rollout unchanged'],
                           'original_binary_sha256': digest(OLD / 'magpie_test_frozen')})


def generate(batch):
    target = D / 'source_games' / f'batch{batch:02}.cgp'
    if target.with_suffix('.done.json').exists():
        return target
    local = LOCAL / target.name
    env = os.environ | {'PCD_GENERATE_OUT': str(local), 'PCD_GENERATE_GAMES': '100',
                        'PCD_GENERATE_MIN_BAG': '5', 'PCD_GENERATE_SEED': str(GEN_SEED + batch * 1000)}
    with target.with_suffix('.log').open('w') as log:
        subprocess.run([str(BIN), 'patcanddiversity'], cwd=REPO, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    shutil.copy2(local, target)
    shutil.copy2(Path(str(local) + '.meta.csv'), Path(str(target) + '.meta.csv'))
    save(target.with_suffix('.done.json'), {'batch': batch, 'games': 100,
                                         'seed': GEN_SEED + batch * 1000,
                                         'finished_epoch': time.time()})
    return target


def source_choices(files):
    choices = []
    for file in sorted(files):
        batch = int(file.stem.replace('batch', ''))
        cgps = {int(line.split(',', 1)[0]): line.split(',', 1)[1]
                for line in read_text_retry(file).splitlines()}
        games = {}
        for row in csv_rows(Path(str(file) + '.meta.csv')):
            cgp = cgps[int(row['pos'])]
            scores = list(map(int, cgp.split()[2].split('/')))
            lead = scores[0] - scores[1]
            if 5 <= int(row['bag']) <= 29 and 40 <= lead <= 80 and int(row['turn']) > 0:
                game = int(row['game'])
                item = {'game': batch * 100 + game, 'source_seed': GEN_SEED + batch * 1000 + game,
                        'batch': batch, 'source_pos': int(row['pos']), 'turn': int(row['turn']),
                        'bag': int(row['bag']), 'phase': 'late', 'score_lead': lead, 'cgp': cgp}
                games.setdefault(game, []).append(item)
        for game in sorted(games):
            rng = random.Random(f'{SAMPLE_SEED}:{batch}:{game}')
            rng.shuffle(games[game])
            choices.append(games[game])
    return choices


def feature_pass(positions, label):
    path = D / f'{label}.cgp'
    path.write_text(''.join(f"{row['pos']},{row['cgp']}\n" for row in positions))
    input_path = snapshot(path)
    processes = []
    for worker in range(WORKERS):
        output = D / f'{label}_features{worker}.csv'
        env = os.environ | COMMON | {'PCD_IN': str(input_path), 'PCD_OUT': str(D / f'{label}_unused{worker}.csv'),
                                    'PCD_FEATURE_ONLY': '1', 'PCD_FEATURE_OUT': str(output),
                                    'PCD_WORKER': str(worker), 'PCD_NUM_WORKERS': str(WORKERS)}
        log = (D / f'{label}_features{worker}.log').open('w')
        processes.append(subprocess.Popen([str(BIN), 'patcanddiversity'], cwd=REPO,
                                           env=env, stdout=log, stderr=subprocess.STDOUT))
        log.close()
    assert all(process.wait() == 0 for process in processes)
    rows = {}
    for worker in range(WORKERS):
        for row in csv_rows(D / f'{label}_features{worker}.csv'):
            rows.setdefault(int(row['pos']), []).append(row)
    return rows


def prepare():
    if (D / 'prepared.json').exists():
        return
    build()
    (D / 'source_games').mkdir(exist_ok=True)
    protocol = {'user_request': 'Include five-in-bag positions; on-turn lead between40and80points.',
                'planned_positions': N, 'one_position_per_independent_game': True,
                'population': 'Fresh CSW24 no-PAT static games; occupied board; bag5–29inclusive; on-turn lead40–80inclusive; at least50static non-pass candidates.',
                'sampling': 'Fresh game seeds, ordered by batch and game. Randomized eligible-position order within game, first position with at least50static candidates. First400eligible independent games. No sim outcome or win-probability filtering.',
                'primary': 'Blocking pool versus exactly equal-count static prefix, measured by selected-play win probability in an independent60s4ply reference.',
                'selection_ms': 15000, 'reference_ms': 60000, 'rollout': 'no-PAT static', 'plies': 4,
                'blocking_formula': 'Unchanged: static equity +1.4*mean(pass opponent best placement score minus candidate opponent best placement score),64paired sampled racks.',
                'blocking_admission': 'Static top25 union adjusted top25; adaptive count.',
                'candidate_universe': 'Top120static plus up to5exchanges within35points of top play; exchanges permitted only when legal.',
                'same_count_control': 'Static prefix containing exactly as many candidates as blocking. All identical candidate pools reuse the same completed15s result.',
                'arm_order': 'Rotate three selection arms by position ID modulo3; same selection seed, independent reference seed.',
                'analysis': 'Fixed N400, include unchanged choices. Two-sided paired t alpha.05;95% position bootstrap. No changes based on significance. Bag5–6/7–14/15–29and lead40–59/60–80are exploratory.',
                'exploratory': ['blocking versus static25', 'equal-count static versus static25', 'pool additions and choices beyond same-size static prefix'],
                'deeper_validation': 'Up to24positions where any selection chooses outside static25; hash-selected without reference gains. Independent600s16ply reference over all three pools; enriched subset only.',
                'interpretation': 'A new targeted follow-up prompted after the prior results; position-level sim study, not measured played-game strength.',
                'seeds': {'games': GEN_SEED, 'sample': SAMPLE_SEED, 'sim': SIM_SEED, 'deep': DEEP_SEED},
                'binary_sha256': digest(BIN), 'harness_sha256': digest(D / 'harness_snapshot.c'),
                'fixed_before_selection_sims_epoch': time.time()}
    save(D / 'PROTOCOL.json', protocol)
    batch_count = 0
    while True:
        stage('generating_fresh_games', batches=batch_count)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(generate, range(batch_count, batch_count + 4)))
        batch_count += 4
        all_choices = source_choices((D / 'source_games').glob('*.cgp'))
        stage('sampling', source_games=batch_count * 100, eligible_games=len(all_choices))
        if len(all_choices) >= N + 40:
            break
        assert batch_count < 80, 'Too few qualifying independent games.'
    remaining = list(all_choices)
    chosen = []
    alternatives = {}
    used_games = set()
    def take(pos):
        candidates = remaining.pop(0)
        record = candidates.pop(0) | {'pos': pos}
        assert record['game'] not in used_games
        used_games.add(record['game'])
        alternatives[pos] = candidates
        return record
    for pos in range(1, N + 1):
        chosen.append(take(pos))
    features = feature_pass(chosen, 'preflight')
    exclusions = []
    attempt = 0
    while True:
        bad = [row for row in chosen if sum(int(f['pool_index']) == int(f['global_static_rank'])
                                           for f in features.get(row['pos'], [])) < 50]
        if not bad:
            break
        for row in bad:
            exclusions.append({key: value for key, value in row.items() if key != 'cgp'} | {'reason': 'fewer than50static candidates'})
            pos = row['pos']
            chosen[pos - 1] = (alternatives[pos].pop(0) | {'pos': pos}) if alternatives[pos] else take(pos)
        attempt += 1
        features.update(feature_pass([chosen[row['pos'] - 1] for row in bad], f'replacement{attempt}'))
    assert len({row['source_seed'] for row in chosen}) == N
    assert any(row['bag'] == 5 for row in chosen), 'No exact-five position in the sample; inspect source coverage.'
    random.Random(SAMPLE_SEED).shuffle(chosen)
    (D / 'positions.cgp').write_text(''.join(f"{row['pos']},{row['cgp']}\n" for row in chosen))
    with (D / 'positions.meta.csv').open('w') as out:
        writer = csv.DictWriter(out, fieldnames=[key for key in chosen[0] if key != 'cgp'])
        writer.writeheader()
        writer.writerows({key: value for key, value in row.items() if key != 'cgp'} for row in chosen)
    with (D / 'candidate_features.csv').open('w') as out:
        writer = csv.DictWriter(out, fieldnames=list(features[1][0]))
        writer.writeheader()
        for pos in sorted(features):
            writer.writerows(features[pos])
    save(D / 'sampling_exclusions.json', exclusions)
    save(D / 'input_hashes.json', {name: digest(D / name) for name in
                                 ['positions.cgp', 'positions.meta.csv', 'candidate_features.csv', 'PROTOCOL.json']})
    save(D / 'prepared.json', {'positions': N, 'independent_games': N, 'source_games': batch_count * 100,
                              'bag_counts': {str(bag): sum(row['bag'] == bag for row in chosen) for bag in range(5,30)},
                              'lead_range': [min(row['score_lead'] for row in chosen), max(row['score_lead'] for row in chosen)],
                              'exclusions': len(exclusions), 'finished_epoch': time.time()})
    stage('prepared', **json.loads(read_text_retry(D / 'prepared.json')))


def run_workers():
    input_path = snapshot(D / 'positions.cgp')
    runtime_binary = executable()
    processes = []
    existing = load(D.glob('w[0-7].csv'))
    lines = read_text_retry(D / 'positions.cgp').splitlines()
    for worker in range(WORKERS):
        remaining = [line for index, line in enumerate(lines) if index % WORKERS == worker
                     and not full(existing.get(int(line.split(',',1)[0]), {}), ARMS)]
        if not remaining:
            continue
        source = D / f'w{worker}.remaining.cgp'
        source.write_text('\n'.join(remaining) + '\n')
        env = os.environ | COMMON | {'PCD_IN': str(snapshot(source)), 'PCD_OUT': str(D / f'w{worker}.csv'),
                                    'PCD_CHECK_OUT': str(D / f'w{worker}_checks.csv'),
                                    'PCD_WORKER': '0', 'PCD_NUM_WORKERS': '1'}
        log = (D / f'w{worker}.log').open('a')
        process = subprocess.Popen([str(runtime_binary), 'patcanddiversity'], cwd=REPO, env=env,
                                   stdout=log, stderr=subprocess.STDOUT)
        log.close()
        processes.append((worker, process))
    save(D / 'primary_pids.json', {str(worker): process.pid for worker, process in processes})
    while any(process.poll() is None for _, process in processes):
        failures = [(worker, process.returncode) for worker, process in processes if process.poll() not in (None,0)]
        if failures:
            for _, process in processes:
                if process.poll() is None:
                    process.terminate()
            for _, process in processes:
                process.wait()
            raise RuntimeError(f'Worker failures: {failures}')
        records = load(D.glob('w[0-7].csv'))
        stage('primary', completed_positions=sum(full(arms, ARMS) for arms in records.values()), planned_positions=N)
        time.sleep(50)
    failures = [(worker, process.returncode) for worker, process in processes if process.returncode != 0]
    if failures:
        raise RuntimeError(f'Worker failures: {failures}; completed data preserved.')
    records = load(D.glob('w[0-7].csv'))
    assert len(records) == N and all(full(arms, ARMS) for arms in records.values())
    save(D / 'primary_complete.json', {'positions': N, 'finished_epoch': time.time()})
    return records


def run_deep(records):
    runtime_binary = executable()
    cgps = {int(line.split(',',1)[0]): line for line in read_text_retry(D / 'positions.cgp').splitlines()}
    targets = [pos for pos, arms in records.items()
               if any(selected(arms[arm]) not in arms['static25'] for arm in ARMS[:-1])]
    targets.sort(key=lambda pos: hashlib.sha256(f'{DEEP_SEED}:{pos}'.encode()).hexdigest())
    targets = targets[:24]
    requests = []
    for pos in targets:
        moves = set().union(*(set(records[pos][arm]) for arm in ARMS[:-1]))
        requests.append({'pos': pos, 'ranks': sorted(int(records[pos]['reference'][move]['static_rank']) for move in moves)})
    save(D / 'deep_manifest.json', {'targets': requests, 'selection': 'Any selected play outside static25; SHA256(deep_seed,pos), maximum24; no reference outcomes used.',
                                  'reference_ms': 600000, 'plies': 16})
    for offset in range(0,len(requests),WORKERS):
        stage('deep_validation', wave=offset//WORKERS+1, planned=len(requests))
        processes = []
        for request in requests[offset:offset+WORKERS]:
            pos = request['pos']
            output = D / f'new{pos}_all25_p16.csv'
            existing = load([output])
            if full(existing.get(pos, {}), ['reference']):
                continue
            source = D / f'new{pos}.cgp'
            source.write_text(cgps[pos] + '\n')
            env = os.environ | COMMON | {'PCD_IN': str(snapshot(source)), 'PCD_OUT': str(output),
                                        'PCD_VALIDATE_RANKS': ','.join(map(str, request['ranks'])),
                                        'PCD_REF_MS': '600000', 'PCD_PLIES': '16', 'PCD_SEED': str(DEEP_SEED)}
            log = (D / f'new{pos}_all25_p16.log').open('a')
            process = subprocess.Popen([str(runtime_binary), 'patcanddiversity'], cwd=REPO, env=env,
                                       stdout=log, stderr=subprocess.STDOUT)
            log.close()
            processes.append((pos,process))
        for pos, process in processes:
            assert process.wait() == 0, f'Deep failure at {pos}'
    save(D / 'data_complete.json', {'positions': N, 'deep_positions': targets, 'finished_epoch': time.time()})


def paired(records, left, right):
    output = []
    for pos, arms in sorted(records.items()):
        lm = selected(arms[left]); rm = selected(arms[right])
        lr = arms['reference'][lm]; rr = arms['reference'][rm]
        output.append({'pos': pos, 'left_move': lm, 'right_move': rm,
                       'wp_gain_pp': 100*(float(lr['sim_wp'])-float(rr['sim_wp'])),
                       'eq_gain': float(lr['sim_eq'])-float(rr['sim_eq']),
                       'reference_mc_sem_upper_pp': 0 if lm == rm else 100*(float(lr['sim_wp_sem'])+float(rr['sim_wp_sem']))})
    return output


def summarize(rows):
    values = [row['wp_gain_pp'] for row in rows]
    mean = statistics.mean(values) if values else None
    se = statistics.stdev(values)/math.sqrt(len(values)) if len(values)>1 else None
    return {'positions': len(rows), 'mean_wp_gain_pp': mean, 'position_se_pp': se,
            'bootstrap95_pp': interval(values),
            't_test_two_sided_p': (t_two_sided(mean/se,len(values)-1) if se else
                                   (1.0 if mean == 0 else 0.0)) if len(values)>1 else None,
            'different_choices': sum(row['left_move'] != row['right_move'] for row in rows),
            'positive': sum(value>0 for value in values), 'negative': sum(value<0 for value in values),
            'zero': sum(value==0 for value in values),
            'reference_mc_sem_upper_mean_pp': math.sqrt(sum(row['reference_mc_sem_upper_pp']**2 for row in rows))/len(rows) if rows else None}


def analyze():
    records = load(D.glob('w[0-7].csv'))
    assert len(records)==N and all(full(arms,ARMS) for arms in records.values())
    meta = {int(row['pos']): row for row in csv_rows(D/'positions.meta.csv')}
    assert len(meta)==N and len({row['source_seed'] for row in meta.values()})==N
    hashes = json.loads(read_text_retry(D/'input_hashes.json'))
    assert all(digest(D/name)==value for name,value in hashes.items())
    protocol = json.loads(read_text_retry(D/'PROTOCOL.json'))
    assert digest(BIN)==protocol['binary_sha256']
    checks = {}
    for file in D.glob('w[0-7]_checks.csv'):
        for row in csv_rows(file):
            checks[int(row['pos']),row['move']] = row
    reused = []
    for pos,arms in records.items():
        assert set(arms['reference']) >= set().union(*(set(arms[arm]) for arm in ARMS[:-1]))
        assert len(arms['threat_wide']) == len(arms['static_blocking_count'])
        count = len(arms['static_blocking_count'])
        assert 25<=count<=50
        assert {int(row['static_rank']) for row in arms['static_blocking_count'].values()} == set(range(1,count+1))
        for arm in ARMS[:-1]:
            for other in ARMS[:-1]:
                if arm<other and set(arms[arm])==set(arms[other]):
                    assert selected(arms[arm])==selected(arms[other])
                    assert all(arms[arm][move]['sim_wp']==arms[other][move]['sim_wp'] for move in arms[arm])
                    assert all(arms[arm][move][key]==arms[other][move][key] for move in arms[arm]
                               for key in ['sim_eq','sim_eq_sem','sim_wp','sim_wp_sem','iterations','chosen'])
                    reused.append({'pos':pos,'arms':[arm,other],'count':len(arms[arm])})
        assert 5<=int(meta[pos]['bag'])<=29 and 40<=int(meta[pos]['score_lead'])<=80
        for move,row in arms['reference'].items():
            check=checks[pos,move]
            assert int(check['racks'])==64 and int(check['conditioned'])==1
            assert 0<=int(check['terminal_replies'])<=64
            assert abs(float(check['pass_reply_mean'])-float(check['candidate_reply_mean'])-float(check['blocking_delta']))<.000003
            assert abs(float(check['blocking_adjustment'])-1.4*float(check['blocking_delta']))<.000003
            assert abs(2*(float(row['threat_value'])-float(row['static_eq']))-float(check['blocking_adjustment']))<.000003
            assert abs(3*(float(row['setup_value'])-float(row['static_eq']))-float(check['setup_adjustment']))<.000003
        for arm in ARMS:
            for row in arms[arm].values():
                assert int(row['bag'])==int(meta[pos]['bag'])
                assert math.isfinite(float(row['sim_wp'])) and 0<=float(row['sim_wp'])<=1
                wall=float(row['wall_ms'])
                if arm=='reference':
                    assert wall>=59500
                elif wall<14500:
                    assert wall<100 and any(other!=arm and set(arms[other])==set(arms[arm])
                                            and all(arms[arm][move]['sim_wp']==arms[other][move]['sim_wp']
                                                    for move in arms[arm]) for other in ARMS[:-1])
    primary_rows=paired(records,'threat_wide','static_blocking_count')
    primary=summarize(primary_rows)
    groups={}
    for label,low,high in [('bag5_6',5,6),('bag7_14',7,14),('bag15_29',15,29)]:
        groups[label]=summarize([row for row in primary_rows if low<=int(meta[row['pos']]['bag'])<=high])
    for label,low,high in [('lead40_59',40,59),('lead60_80',60,80)]:
        groups[label]=summarize([row for row in primary_rows if low<=int(meta[row['pos']]['score_lead'])<=high])
    deep_records=[]
    for file in sorted(D.glob('new*_all25_p16.csv')):
        rows=load([file])
        for pos,arms in rows.items():
            assert full(arms,['reference'])
            ref=arms['reference']
            assert all(float(row['wall_ms'])>=599500 for row in ref.values())
            lm=selected(records[pos]['threat_wide']);rm=selected(records[pos]['static_blocking_count'])
            prefix=records[pos]['static_blocking_count']
            assert all(move in ref for move in set().union(*(set(records[pos][arm]) for arm in ARMS[:-1])))
            deep_records.append({'pos':pos,'left_move':lm,'right_move':rm,
                                 'wp_gain_pp':100*(float(ref[lm]['sim_wp'])-float(ref[rm]['sim_wp'])),
                                 'reference_mc_sem_upper_pp':0 if lm==rm else 100*(float(ref[lm]['sim_wp_sem'])+float(ref[rm]['sim_wp_sem'])),
                                 'gain_vs_best_same_count_static_pp':100*(float(ref[lm]['sim_wp'])-max(float(ref[move]['sim_wp']) for move in prefix))})
    changed=[]
    for row in primary_rows:
        pos=row['pos'];arms=records[pos]
        if row['left_move']!=row['right_move']:
            rank=int(arms['reference'][row['left_move']]['global_static_rank'])
            changed.append(row|{'bag':int(meta[pos]['bag']),'score_lead':int(meta[pos]['score_lead']),
                                'blocking_count':len(arms['threat_wide']),'static_rank':rank,
                                'outside_equal_count_static':row['left_move'] not in arms['static_blocking_count']})
    output={'study':'Blocking at bag5–29 and on-turn lead40–80', 'primary':primary,
            'groups_exploratory':groups,
            'exploratory':{'blocking_vs_static25':summarize(paired(records,'threat_wide','static25')),
                           'same_count_static_vs_static25':summarize(paired(records,'static_blocking_count','static25'))},
            'changed_cases':changed,'deep':{'summary':summarize(deep_records),'per_position':deep_records,
                                          'interpretation':'Outcome-blind novel-choice enriched subset; not a population effect estimate.'},
            'integrity':{'positions':N,'independent_games':len({row['source_seed'] for row in meta.values()}),
                         'input_hashes_valid':True,'binary_hash_valid':True,'all_pools_complete':True,
                         'same_counts_exact':True,'identical_pool_pairs':len(reused),'check_rows':len(checks)},
            'per_position':primary_rows}
    save(D/'summary.json',output)
    save(D/'identical_pool_reuse.json',reused)
    with (D/'paired_outcomes.csv').open('w') as out:
        writer=csv.DictWriter(out,fieldnames=list(primary_rows[0]))
        writer.writeheader();writer.writerows(primary_rows)
    save(D/'result_hashes.json',{file.name:digest(file) for file in sorted(D.glob('*.csv'))})
    print(json.dumps({key:value for key,value in output.items() if key not in ['per_position','changed_cases']},indent=2),flush=True)


def page():
    os.environ['CANDIDATE_DEBUG_SOURCE']=str(D)
    os.environ['CANDIDATE_DATA_ROOT']=str(REPO/'data')
    from build_candidate_debug import dataset
    data=dataset(D,'primary',[f'w{worker}.csv' for worker in range(WORKERS)],['positions.cgp'],ARMS[:-1])
    data['positions'].sort(key=lambda item:item['id'])
    payload={'datasets':[data],
             'premiums':[list(line) for line in read_text_retry(REPO/'data/layouts/standard15.txt').splitlines()[1:]],
             'tileScores':{row[0]:int(row[3]) for row in csv.reader(io.StringIO(read_text_retry(REPO/'data/letterdistributions/english.csv')))}}
    template=read_text_retry(ROOT/'candidate_debug_template.html')
    template=template.replace('Inspect the plays that setup, blocking, and exchange checks add to a static simulation pool.',
                              'Inspect blocking additions alongside a static pool of exactly the same size.')
    template=template.replace('<option value="primary">Legacy setup + blocking · 320 positions</option><option value="quota">Legacy quotas · 40 fresh positions</option>',
                              '<option value="primary">Blocking · bag5–29 · lead40–80 ·400positions</option>')
    template=template.replace("tile_exchange:'Tile + exchange'", "tile_exchange:'Tile + exchange',static_blocking_count:'Static · blocking count'")
    template=template.replace('The pass-relative rerun is in progress; this is the legacy data.',
                              'Fresh targeted blocking study: bag 5–29, on-turn lead 40–80. Full 15-second sims and a static pool of exactly the same size.')
    template=template.replace('Blocking ranks plays by static equity minus 1.4 times the mean best opponent placement reply score. Setup ranks plays by static equity plus 0.75 times the setup component.',
                              'Blocking adds 1.4 times the mean opponent best-placement score reduction versus passing, using 64 paired racks. Setup adjustments are diagnostics; setup does not admit candidates in this study. The static count control is exactly the size of the blocking pool.')
    template=template.replace('setup = 3 × (exported setup value − static equity). A smaller blocking penalty ranks better; a larger setup bonus ranks better.',
                              'setup = 3 × (exported setup value − static equity). Positive blocking reduces opponent scoring versus pass. Setup is diagnostic only.')
    template=template.replace('Primary pool sims have a 15-second maximum and may stop early;', 'Primary pool sims run for a full 15-second budget;')
    template=template.replace('Repaired references replace seven original runs.', 'Independent 600-second sixteen-ply references cover the outcome-blind novel-choice subset.')
    template=template.replace('Other groups contain plays outside that anchor, labeled by every check that admitted them.', 'Other groups contain plays outside that anchor, labeled by blocking admission or membership in the same-size static control. The static control label describes pool membership.')
    template=template.replace(' In the quota study these are diagnostic values.', '')
    template=template.replace(' Raw signals adds up to five pure threat and five pure setup candidates within 25 equity points of the best static play. Exchange adds up to five exchanges within 35 points. Combined takes their union. The fresh quota experiment adds a best placement for each tile count within 35 points, plus the exchange quota.', '')
    template=template.replace('[1217,493,580,1100,2241,1755]', 'dataset.positions.filter(item=>item.candidates.some(candidate=>!candidate.results[base()]&&candidate.results.threat_wide?.[4])).slice(0,6).map(item=>item.id)')
    template=template.replace("dataset.name==='primary'?1217:106", 'dataset.positions[0].id')
    template=template.replace('Setup / blocking study','Blocking · lead40–80')
    template=template.replace("pool==='reference'?'Reference best':'Pool choice'",
                              "pool==='reference'?'4-ply reference choice':'Pool choice'")
    template=template.replace('All rollouts use no-PAT static play.',
                              'Choice labels identify the original four-ply runs; the deep view reevaluates those choices. All rollouts use no-PAT static play.')
    template=template.replace('1217, KiSTFUL, BUSTI…','Position or play…')
    filters='''<label class="field">Tiles in bag<select id="bagFilter"><option value="all">All · 5–29</option><option value="5">Exactly 5</option><option value="5-6">5–6</option><option value="7-14">7–14</option><option value="15-29">15–29</option></select></label><label class="field">On-turn lead<select id="leadFilter"><option value="all">All · 40–80</option><option value="40-59">40–59</option><option value="60-80">60–80</option></select></label><span id="filterCount" class="help"></span>'''
    template=template.replace('<label class="field">Position<select id="position">',filters+'<label class="field">Position<select id="position">')
    filter_logic='''const matchesTargetFilters=item=>{const bag=$('bagFilter').value,lead=$('leadFilter').value,margin=item.scores[item.onturn]-item.scores[1-item.onturn];const inRange=(value,range)=>{if(range==='all')return true;const bounds=range.split('-').map(Number);return value>=bounds[0]&&value<=(bounds[1]??bounds[0]);};return inRange(item.bag,bag)&&inRange(margin,lead);};
'''
    template=template.replace('function filterPositions(){',filter_logic+'function filterPositions(){')
    template=template.replace('visible=dataset.positions.filter(item=>(!query', 'visible=dataset.positions.filter(item=>matchesTargetFilters(item)&&(!query')
    template=template.replace('visible.sort((left,right)=>left.id-right.id);', "visible.sort((left,right)=>left.id-right.id);$('filterCount').textContent=visible.length+' / '+dataset.positions.length+' positions';")
    template=template.replace("$('novelOnly').checked=false;filterPositions();", "$('novelOnly').checked=false;$('bagFilter').value=$('leadFilter').value='all';filterPositions();")
    template=template.replace("$('search').oninput=filterPositions;", "$('search').oninput=filterPositions;$('bagFilter').onchange=filterPositions;$('leadFilter').onchange=filterPositions;")
    (D/'candidate-pools-debug.html').write_text(template.replace('/*DATA*/null',json.dumps(payload,separators=(',',':'))))
    summary=json.loads(read_text_retry(D/'summary.json'))
    primary=summary['primary'];ci=primary['bootstrap95_pp'];p=primary['t_test_two_sided_p']
    rows=''.join(f"<tr><td>{label}</td><td>{record['positions']}</td><td>{record['mean_wp_gain_pp']:.5f}</td><td>{record['different_choices']}</td></tr>" for label,record in summary['groups_exploratory'].items() if record['positions'])
    cases=''.join(f"<tr><td><a href='candidate-pools-debug.html#primary/{row['pos']}'>{row['pos']}</a></td><td>{row['bag']}</td><td>{row['score_lead']}</td><td>{row['left_move']}</td><td>{row['right_move']}</td><td>{row['wp_gain_pp']:+.5f}</td></tr>" for row in sorted(summary['changed_cases'],key=lambda item:-item['wp_gain_pp']))
    p_text=f'{p:.6g}' if p is not None else 'unavailable'
    report=f'''<!doctype html><meta charset="utf-8"><title>Targeted blocking results</title><style>body{{font:16px system-ui;max-width:1100px;margin:40px auto;padding:0 20px;line-height:1.5}}h1{{font-size:30px}}.result{{font-size:24px;background:#eef5ff;padding:20px;border-radius:12px}}table{{border-collapse:collapse;width:100%;margin:24px 0}}th,td{{text-align:left;padding:8px;border-bottom:1px solid #ddd}}</style><h1>Blocking with a 40–80 point lead</h1><p>400 fresh independent CSW24 static-game positions, 5–29 tiles in bag. All candidate selection and rollouts disable PAT.</p><p class="result">Blocking vs same-size static: {primary['mean_wp_gain_pp']:+.5f} percentage points<br>95% position bootstrap: [{ci[0]:+.5f}, {ci[1]:+.5f}]; two-sided p={p_text}</p><p>15-second selection sims; independent 60-second four-ply references. Identical pools share their completed sim. {primary['different_choices']} different choices, {primary['positive']} positive, {primary['negative']} negative, {primary['zero']} zero. <a href="candidate-pools-debug.html">Open sortable candidate explorer</a>.</p><h2>Exploratory bag and lead groups</h2><table><tr><th>Group</th><th>N</th><th>Gain (pp)</th><th>Changed choices</th></tr>{rows}</table><h2>Changed choices</h2><table><tr><th>Position</th><th>Bag</th><th>Lead</th><th>Blocking</th><th>Same-size static</th><th>Gain (pp)</th></tr>{cases}</table><p>Deeper validation: {len(summary['deep']['per_position'])} positions with novel choices, independent 600-second sixteen-ply references. This enriched subset cannot estimate the full population effect. This targeted follow-up was prompted after the prior study; no analysis changes based on significance. These are position-level sim results, not played-game strength. Candidate eligibility requires at least 50 static non-pass moves.</p><details><summary>Full audited summary</summary><pre>{json.dumps(summary,indent=2)}</pre></details>'''
    def result_row(label, result):
        bounds = result['bootstrap95_pp']
        bounds_text = (f"[{bounds[0]:+.5f}, {bounds[1]:+.5f}]"
                       if bounds is not None else 'Unavailable')
        probability = result['t_test_two_sided_p']
        probability_text = f'{probability:.6g}' if probability is not None else 'Unavailable'
        mean = result['mean_wp_gain_pp']
        mean_text = f'{mean:+.5f}' if mean is not None else 'Unavailable'
        return (f"<tr><td>{label}</td><td>{result['positions']}</td>"
                f"<td>{mean_text}</td><td>{bounds_text}</td><td>{probability_text}</td></tr>")
    comparison_rows = result_row('Blocking vs same-size static (primary)', primary)
    comparison_rows += result_row('Blocking vs static 25 (exploratory)',
                                  summary['exploratory']['blocking_vs_static25'])
    comparison_rows += result_row('Same-size static vs static 25 (exploratory)',
                                  summary['exploratory']['same_count_static_vs_static25'])
    comparison_table = ("<h2>Candidate-pool comparisons</h2><table><tr><th>Comparison</th>"
                        "<th>N</th><th>Gain (pp)</th><th>95% position bootstrap</th>"
                        f"<th>Two-sided p</th></tr>{comparison_rows}</table>"
                        "<p>The primary comparison controls for candidate count. The other two "
                        "comparisons show the combined effect of blocking and pool width, and the "
                        "effect of widening the static pool alone. Exploratory comparisons and "
                        "subgroups are not adjusted for multiple testing.</p>")
    report = report.replace('<h2>Exploratory bag and lead groups</h2>',
                            comparison_table + '<h2>Exploratory bag and lead groups</h2>')
    primary_test_note = ('<p><b>Primary test:</b> The prespecified paired t-test ' +
                         ('reaches' if p is not None and p < .05 else 'does not reach') +
                         ' the 5% significance threshold.</p>')
    if p is not None and p >= .05 and ci is not None and ci[0] > 0:
        primary_test_note += ('<p>The percentile bootstrap interval barely excludes zero, '
                              'while the paired t-test does not reject zero. These procedures '
                              f'differ for this sparse result with {primary["different_choices"]} '
                              'changed choices. The prespecified t-test determines the '
                              'primary significance conclusion.</p>')
    report = report.replace('<h2>Candidate-pool comparisons</h2>',
                            primary_test_note + '<h2>Candidate-pool comparisons</h2>')
    if summary['deep']['per_position']:
        deep_table = ("<h2>Longer-reference check</h2><table><tr><th>Comparison</th>"
                      "<th>N</th><th>Gain (pp)</th><th>95% position bootstrap</th>"
                      "<th>Two-sided p</th></tr>" +
                      result_row('Blocking vs same-size static; novel-choice subset',
                                 summary['deep']['summary']) + "</table>"
                      "<p>These positions were selected by a fixed hash from cases where an arm "
                      "chose outside static 25, without using reference gains. This check uses "
                      "independent 600-second, sixteen-ply references; its mean describes this "
                      "enriched subset only.</p>")
        report = report.replace('<details><summary>Full audited summary</summary>',
                                deep_table + '<details><summary>Full audited summary</summary>')
    # Keep presentation formatting separate from numerical calculations.
    (D/'REPORT.html').write_text(report)
    save(D/'deliverable_hashes.json',{name:digest(D/name) for name in ['REPORT.html','candidate-pools-debug.html','summary.json']})
    print('Built targeted explorer and report.',flush=True)


def main():
    D.mkdir(exist_ok=True)
    lock=(D/'coordinator.lock').open('a')
    fcntl.flock(lock.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
    save(D/'driver_pid.json',{'pid':os.getpid(),'started_epoch':time.time()})
    if (D/'complete.json').exists():
        print('Already complete; no duplicate study.')
        return
    mode=sys.argv[1] if len(sys.argv)>1 else 'run'
    if mode=='build':
        build();return
    if mode=='prepare':
        prepare();return
    if mode=='analyze':
        analyze();return
    if mode=='page':
        page();return
    prepare()
    validation=json.loads(read_text_retry(D/'preflight_validation.json'))
    assert validation['old_new_candidate_membership_exact'] and validation['old_new_adjustments_exact']
    assert validation['same_count_control_exact'] and validation['original_binary_unchanged']
    hashes=json.loads(read_text_retry(D/'input_hashes.json'))
    assert all(digest(D/name)==value for name,value in hashes.items())
    records=run_workers()
    run_deep(records)
    stage('analysis')
    analyze()
    page()
    save(D/'complete.json',{'positions':N,'finished_epoch':time.time(),
                          'page':str(D/'candidate-pools-debug.html'),'report':str(D/'REPORT.html')})
    stage('complete',positions=N)


if __name__=='__main__':
    try:
        main()
    except Exception as error:
        save(D/'failure.json',{'epoch':time.time(),'error':repr(error),'pid':os.getpid()})
        raise
