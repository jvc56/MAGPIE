#!/usr/bin/env python3
"""Durable deadline-bounded driver; never starts competing suites."""
import csv
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from overnight_prepare import BIN, D, DEADLINE, PREVIOUS, REPO, SIM_SEED, save

COMMON = {'PCD_PASS_RELATIVE': '1', 'PCD_ADAPTIVE': '1', 'PCD_EXPLORE': '1',
          'PCD_POOL': '120', 'PCD_EXCHANGE_QUOTA': '5', 'PCD_EXCHANGE_MARGIN': '35',
          'PCD_CONDITION_DRAWS': '1', 'PCD_MS': '15000', 'PCD_REF_MS': '60000',
          'PCD_PLIES': '4', 'PCD_SELECTION_NO_CUTOFF': '1', 'PCD_SEED': str(SIM_SEED)}

LOCAL_INPUTS = Path('/private/tmp/magpie-overnight-confirm-20260930-inputs')


def read_bytes_retry(file):
    for attempt in range(3):
        try:
            return file.read_bytes()
        except OSError:
            if attempt == 2:
                raise
            time.sleep(1)


def read_text_retry(file):
    return read_bytes_retry(file).decode()


def local_snapshot(file):
    """Keep frozen bytes on the local volume for repeated C stdio reads."""
    data = read_bytes_retry(file)
    digest = hashlib.sha256(data).hexdigest()
    LOCAL_INPUTS.mkdir(exist_ok=True)
    target = LOCAL_INPUTS / f'{digest}-{file.name}'
    if not target.exists():
        target.write_bytes(data)
    assert hashlib.sha256(target.read_bytes()).hexdigest() == digest
    return target


def csv_rows(file):
    return csv.DictReader(io.StringIO(read_text_retry(file)))


def load(files):
    data = {}
    for file in files:
        if not file.exists():
            continue
        for row in csv_rows(file):
            if None not in row and row.get('iterations') and row.get('global_static_rank'):
                data.setdefault(int(row['pos']), {}).setdefault(row['arm'], {})[row['move']] = row
    return data


def full(arms, required):
    return all(arm in arms and len(arms[arm]) == int(next(iter(arms[arm].values()))['candidates'])
               and sum(int(row['chosen']) for row in arms[arm].values()) == 1 for arm in required)


def selected(rows):
    choices = [move for move, row in rows.items() if int(row['chosen'])]
    assert len(choices) == 1
    return choices[0]


def stage(name, **extra):
    save(D / 'progress.json', {'stage': name, 'epoch': time.time(), 'driver_pid': os.getpid(), **extra})
    print(json.dumps({'stage': name, **extra}), flush=True)


def group(label, prefix, positions, extra, required):
    processes = []
    stage(label)
    lines = read_text_retry(positions).splitlines()
    completed_before = load(D.glob(f'{prefix}[0-7].csv')) if label == 'matched' else {}
    if label == 'matched':
        extra = extra | {'PCD_MATCHED_COUNTS': str(local_snapshot(Path(extra['PCD_MATCHED_COUNTS'])))}
        resumes = []
    for worker in range(8):
        marker = D / f'{prefix}{worker}.done.json'
        if marker.exists():
            continue
        input_path = positions
        worker_settings = {'PCD_WORKER': str(worker), 'PCD_NUM_WORKERS': '8'}
        if label == 'matched':
            remaining = [line for index, line in enumerate(lines) if index % 8 == worker
                         and not full(completed_before.get(int(line.split(',', 1)[0]), {}), required)]
            resumes.append({'worker': worker, 'remaining': len(remaining),
                            'positions': [int(line.split(',', 1)[0]) for line in remaining]})
            if not remaining:
                save(marker, {'finished_epoch': time.time(), 'exit': 0, 'all_positions_already_complete': True})
                continue
            source = D / f'm{worker}.remaining.cgp'
            source.write_text('\n'.join(remaining) + '\n')
            input_path = local_snapshot(source)
            worker_settings = {'PCD_WORKER': '0', 'PCD_NUM_WORKERS': '1'}
        env = os.environ | COMMON | extra | {'PCD_IN': str(input_path), 'PCD_OUT': str(D / f'{prefix}{worker}.csv'),
                                            'PCD_CHECK_OUT': str(D / f'{prefix}{worker}_checks.csv'),
                                            **worker_settings}
        log = (D / f'{prefix}{worker}.log').open('a')
        proc = subprocess.Popen([str(BIN), 'patcanddiversity'], cwd=REPO, env=env,
                                stdout=log, stderr=subprocess.STDOUT)
        log.close(); processes.append((worker, proc))
    save(D / f'{label}_pids.json', {str(worker): process.pid for worker, process in processes})
    if label == 'matched':
        save(D / 'matched_resume.json', {'epoch': time.time(), 'workers': resumes,
                                       'counts_snapshot': extra['PCD_MATCHED_COUNTS']})
    while any(process.poll() is None for _, process in processes):
        failed = [(worker, process.returncode) for worker, process in processes
                  if process.poll() not in (None, 0)]
        if failed:
            for _, process in processes:
                if process.poll() is None:
                    process.terminate()
            for _, process in processes:
                process.wait()
            raise RuntimeError(f'{label}: worker failures {failed}; completed records preserved')
        data = load(D.glob(f'{prefix}[0-7].csv'))
        stage(label, completed_positions=sum(full(arms, required) for arms in data.values()),
              planned_positions=len(lines))
        time.sleep(50)
    failures = []
    for worker, process in processes:
        if process.returncode:
            failures.append({'worker': worker, 'exit': process.returncode})
        else:
            save(D / f'{prefix}{worker}.done.json', {'finished_epoch': time.time(), 'exit': 0})
    if failures:
        raise RuntimeError(f'{label}: {failures}')
    data = load(D.glob(f'{prefix}[0-7].csv'))
    completed = {pos: arms for pos, arms in data.items() if full(arms, required)}
    save(D / f'{label}_complete.json', {'finished_epoch': time.time(), 'positions': len(completed)})
    return completed


def main():
    D.mkdir(exist_ok=True)
    coordinator_lock = (D / 'coordinator.lock').open('a')
    try:
        fcntl.flock(coordinator_lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print('An overnight coordinator is already active. No duplicate run started.')
        return
    save(D / 'driver_pid.json', {'pid': os.getpid(), 'started_epoch': time.time()})
    if (D / 'complete.json').exists():
        print('Already complete. No duplicate runs.'); return
    stage('waiting_for_preparation_and_prior_rerun')
    while not (D / 'prepared.json').exists() or not (PREVIOUS / 'complete.json').exists():
        if time.time() >= DEADLINE - 7200:
            raise RuntimeError('Insufficient time to begin the frozen main suite before its deadline.')
        time.sleep(30)
    protocol = json.loads((D / 'PROTOCOL.json').read_text())
    assert hashlib.sha256(BIN.read_bytes()).hexdigest() == protocol['binary_sha256']
    primary = group('primary', 'w', D / 'positions.cgp', {'PCD_DEADLINE': str(DEADLINE - 7200)},
                    ['static25', 'threat_wide', 'setup_wide', 'signals', 'exchange', 'combined', 'reference'])
    assert primary
    positions = {int(line.split(',', 1)[0]): line for line in (D / 'positions.cgp').read_text().splitlines()}
    features = {}
    for row in csv.DictReader((D / 'candidate_features.csv').open()):
        features.setdefault(int(row['pos']), []).append(row)
    exceptions = []
    with (D / 'matched_counts.csv').open('w') as out:
        out.write('pos,setup_count,combined_count\n')
        for pos, arms in primary.items():
            setup_count = len(arms['setup_wide']); combined_count = len(arms['combined'])
            core = sum(int(row['global_static_rank']) == int(row['pool_index']) for row in features[pos])
            assert setup_count >= 25 and setup_count <= core
            if combined_count > core:
                exceptions.append({'pos': pos, 'combined_count': combined_count, 'available_static_count': core})
            out.write(f'{pos},{setup_count},{min(combined_count, core)}\n')
    save(D / 'combined_count_exceptions.json', exceptions)
    (D / 'matched_positions.cgp').write_text(''.join(positions[pos] + '\n' for pos in positions if pos in primary))
    matched = group('matched', 'm', D / 'matched_positions.cgp',
                    {'PCD_MATCHED_ONLY': '1', 'PCD_MATCHED_COUNTS': str(D / 'matched_counts.csv'),
                     'PCD_DEADLINE': str(DEADLINE - 900)}, ['static_setup_count', 'static_combined_count'])
    # Identical candidate sets represent the same policy. Reuse its original
    # full-budget result rather than count timing noise as candidate diversity.
    reuse = []
    for pos, arms in matched.items():
        for arm, rows in list(arms.items()):
            source = next((name for name in ['static25', 'setup_wide', 'threat_wide', 'signals', 'exchange', 'combined']
                           if set(primary[pos][name]) == set(rows)), None)
            if source is not None:
                matched[pos][arm] = {move: row | {'arm': arm, 'wall_ms': '0.000'}
                                     for move, row in primary[pos][source].items()}
                reuse.append({'pos': pos, 'arm': arm, 'source_arm': source, 'candidates': len(rows)})
    with (D / 'matched_effective.csv').open('w') as out:
        example = next(iter(next(iter(matched.values())).values()))
        writer = csv.DictWriter(out, fieldnames=list(next(iter(example.values()))))
        writer.writeheader()
        for arms in matched.values():
            for rows in arms.values():
                writer.writerows(rows.values())
    save(D / 'matched_identical_reuse.json', reuse)
    eligible = sorted(set(primary) & set(matched))
    novel = [pos for pos in eligible if any(selected(rows) not in primary[pos]['static25']
                                          for arm, rows in (primary[pos] | matched[pos]).items() if arm != 'reference')]
    novel.sort(key=lambda pos: hashlib.sha256(f'202609300703:{pos}'.encode()).hexdigest())
    targets = novel[:80]
    requests = []
    for pos in targets:
        moves = set(primary[pos]['static25']) | set(matched[pos]['static_setup_count']) | set(matched[pos]['static_combined_count'])
        moves.update(selected(rows) for arm, rows in primary[pos].items() if arm != 'reference')
        moves.update(selected(rows) for rows in matched[pos].values())
        ranks = sorted(int(primary[pos]['reference'][move]['static_rank']) for move in moves)
        inputfile = D / f'new{pos}.cgp'; inputfile.write_text(positions[pos] + '\n')
        requests.append({'pos': pos, 'ranks': ranks})
    save(D / 'deep_manifest.json', {'eligible_novel_positions': len(novel), 'planned': requests,
                                  'selection': protocol['deep_selection'], 'reference_ms': 600000, 'plies': 16})
    completed_deep = []
    for offset in range(0, len(requests), 8):
        if time.time() + 660 > DEADLINE:
            break
        stage('deep_validation', wave=offset // 8 + 1, planned=len(requests), completed=len(completed_deep))
        processes = []
        for request in requests[offset:offset + 8]:
            pos = request['pos']; outfile = D / f'new{pos}_all25_p16.csv'
            existing = load([outfile])
            if pos in existing and full(existing[pos], ['reference']):
                completed_deep.append(pos); continue
            env = os.environ | COMMON | {'PCD_IN': str(D / f'new{pos}.cgp'), 'PCD_OUT': str(outfile),
                                        'PCD_VALIDATE_RANKS': ','.join(map(str, request['ranks'])),
                                        'PCD_REF_MS': '600000', 'PCD_PLIES': '16', 'PCD_SEED': '202609300703'}
            log = (D / f'new{pos}_all25_p16.log').open('a')
            proc = subprocess.Popen([str(BIN), 'patcanddiversity'], cwd=REPO, env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
            log.close(); processes.append((pos, proc))
        for pos, process in processes:
            assert process.wait() == 0, f'Deep reference failed at{pos}'
            completed_deep.append(pos)
    save(D / 'data_complete.json', {'finished_epoch': time.time(), 'planned_positions': 1200,
                                   'primary_positions': len(primary), 'matched_positions': len(matched),
                                   'paired_positions': len(eligible), 'full_confirmation_sample': len(eligible) == 1200,
                                   'deep_completed': completed_deep, 'deep_planned': targets})
    stage('analysis_and_page')
    python = '/Users/olaugh/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3'
    subprocess.run([python, str(D.parent / 'overnight_analyze.py')], check=True)
    subprocess.run([sys.executable, str(D.parent / 'overnight_build_page.py')], check=True)
    save(D / 'complete.json', {'finished_epoch': time.time(), 'paired_positions': len(eligible),
                             'full_confirmation_sample': len(eligible) == 1200,
                             'report': str(D / 'REPORT.html'), 'page': str(D / 'candidate-pools-debug.html')})
    stage('complete', paired_positions=len(eligible))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        save(D / 'failure.json', {'epoch': time.time(), 'error': repr(error), 'pid': os.getpid()})
        raise
