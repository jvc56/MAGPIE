#!/usr/bin/env python3
"""Run independent, clocked inference-match workers until a fixed deadline.

The C infmatch harness charges inference and simulation to each player's game
clock. Only completed game pairs enter the reported win-rate confidence interval.
This runner never stops based on the observed win rate.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import signal
import statistics
import subprocess
import time


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def summarize(directory, started):
    games, pairs, inference_seconds = [], [], []
    for path in sorted(directory.glob('worker-*.log')):
        for line in path.read_text(errors='replace').splitlines():
            if line.startswith('INFMATCH_GAME '):
                row = dict(re.findall(r'(\w+)=([^ ]+)', line))
                if 'control_clock_stops' in row:
                    games.append(row)
            elif line.startswith('INFMATCH_INFERENCE '):
                row = dict(re.findall(r'(\w+)=([^ ]+)', line))
                if 'seconds' in row:
                    inference_seconds.append(float(row['seconds']))
            elif line.startswith('INFMATCH_PAIR '):
                row = dict(re.findall(r'(\w+)=([^ ]+)', line))
                if 'spread' in row:
                    pairs.append(row)
    result = {'updated_utc': datetime.now(timezone.utc).isoformat(),
              'elapsed_seconds': time.monotonic() - started,
              'completed_games_in_logs': len(games), 'completed_pairs': len(pairs),
              'games_in_primary_analysis': 2 * len(pairs)}
    if inference_seconds:
        result['inference_seconds_max'] = max(inference_seconds)
        result['inference_seconds_mean'] = statistics.mean(inference_seconds)
        result['timed_inference_calls'] = len(inference_seconds)
    if pairs:
        seeds = [row['seed'] for row in pairs]
        if len(set(seeds)) != len(seeds):
            raise RuntimeError('Duplicate game-pair seeds')
        outcomes = [float(row['win_fraction']) for row in pairs]
        mean = statistics.mean(outcomes)
        result['softmax_win_fraction_ties_half'] = mean
        result['mean_spread_per_game'] = statistics.mean(float(row['spread']) for row in pairs)
        if len(pairs) > 1:
            variance = statistics.variance(outcomes)
            half_width = 1.959963984540054 * math.sqrt(variance / len(pairs))
            result['pair_variance'] = variance
            result['paired_normal_95_ci'] = [max(0, mean-half_width), min(1, mean+half_width)]
    if games:
        for key in ('soft_seconds', 'control_seconds', 'soft_iterations', 'control_iterations',
                    'soft_fallbacks', 'control_fallbacks', 'infer_attempts', 'inferred_sims',
                    'soft_clock_stops', 'control_clock_stops', 'infer_skips',
                    'soft_emergency_passes', 'control_emergency_passes'):
            result[key + '_mean'] = statistics.mean(float(row.get(key, 0)) for row in games)
        for side in ('soft', 'control'):
            result[side + '_seconds_max'] = max(float(row[side + '_seconds']) for row in games)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--directory', required=True, type=Path)
    parser.add_argument('--stop-at', required=True)
    parser.add_argument('--workers', type=int, default=18)
    parser.add_argument('--budget-ms', type=int, default=15000)
    parser.add_argument('--seed', type=int, default=2026110300)
    args = parser.parse_args()
    deadline = datetime.fromisoformat(args.stop_at)
    if deadline.tzinfo is None or deadline.timestamp() <= time.time():
        parser.error('--stop-at must be a future timezone-aware timestamp')
    if args.workers < 1 or args.budget_ms < 1:
        parser.error('positive worker count and game budget required')
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    if list(directory.glob('worker-*.log')):
        parser.error('directory already contains match logs; refusing to overwrite')
    binary = args.binary.resolve()
    manifest = {'start_utc': datetime.now(timezone.utc).isoformat(), 'stop_at': deadline.isoformat(),
                'workers': args.workers, 'game_budget_ms_per_player': args.budget_ms,
                'seed': args.seed, 'binary': str(binary),
                'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                'lexicon': 'CSW24', 'wmp': True, 'rit': True, 'wit': True,
                'bag_cycle': True, 'sim_plies': 2, 'candidates': 15,
                'softmax_margin': 20, 'softmax_temperature': 5,
                'control_inference': False, 'PAT': False, 'game_pairs': True,
                'inference_charged_to_game_clock': True,
                'reserve_seconds': min(1.0, args.budget_ms / 10000),
                'inference_min_search_seconds': 3.0,
                'overrun_policy': 'abort_experiment_before_playing_late_move',
                'pilot_in_primary_analysis': False}
    write_json(directory/'manifest.json', manifest)
    stopping = False
    def stop(_signum, _frame):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    started = time.monotonic()
    jobs = []
    reason = 'deadline'
    failure = None
    try:
        for worker in range(args.workers):
            log = open(directory/f'worker-{worker:02d}.log', 'w')
            spec = f'infmatch:100000:{args.budget_ms}:{args.seed+worker}:{args.workers}:0'
            proc = subprocess.Popen([str(binary), spec], stdout=log, stderr=subprocess.STDOUT,
                                    cwd=Path(__file__).resolve().parents[1])
            jobs.append((proc, log))
        write_json(directory/'processes.json', {'runner_pid': __import__('os').getpid(),
                                               'worker_pids': [p.pid for p, _ in jobs]})
        while not stopping and time.time() < deadline.timestamp():
            for proc, _ in jobs:
                if proc.poll() is not None:
                    raise RuntimeError(f'worker {proc.pid} exited unexpectedly: {proc.returncode}')
            summary = summarize(directory, started)
            summary['status'] = 'running'
            write_json(directory/'summary.json', summary)
            time.sleep(min(15, max(0, deadline.timestamp()-time.time())))
        if stopping:
            reason = 'interrupted'
    except Exception as error:
        reason, failure = 'failed', str(error)
    finally:
        for proc, _ in jobs:
            if proc.poll() is None:
                proc.terminate()
        for proc, log in jobs:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            log.close()
        summary = summarize(directory, started)
        summary.update(status=reason, error=failure,
                       note='Only complete pairs enter the primary analysis; unfinished pairs at the deadline are excluded.')
        write_json(directory/'summary.json', summary)
        print(json.dumps(summary, indent=2), flush=True)
    return 1 if failure else 0


if __name__ == '__main__':
    raise SystemExit(main())
