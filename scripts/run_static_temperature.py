#!/usr/bin/env python3
"""Run ascending static-inference temperature validation, saving each stage."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import time
from summarize_static_temperature import summarize


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('bin/magpie_test'))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--workers', type=int, default=18)
    parser.add_argument('--pairs-per-shard', type=int, default=6)
    parser.add_argument('--sizes', type=int, nargs='+', default=[1, 3, 4, 5, 6, 7])
    parser.add_argument('--size-pairs', nargs='*', default=[], help='Override pairs per shard, e.g. 1:18 6:24 7:24')
    parser.add_argument('--seed', type=int, default=2026310000)
    args = parser.parse_args()
    size_pairs = dict(tuple(map(int, value.split(':'))) for value in args.size_pairs)
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    binary_sha256 = hashlib.sha256(binary.read_bytes()).hexdigest()
    metadata = vars(args).copy()
    metadata.update(binary=str(binary), binary_sha256=binary_sha256, output=str(args.output), timing='wall time under concurrent single-thread jobs', started=time.time())
    manifest = args.output / 'run.json'
    if manifest.exists():
        previous = json.loads(manifest.read_text())
        for key in ('binary_sha256', 'workers', 'pairs_per_shard', 'sizes',
                    'size_pairs', 'seed'):
            if previous.get(key) != metadata[key]:
                raise ValueError(f'Existing run differs in {key}; use a new output directory')
        if (args.output / 'complete.json').exists():
            for size in args.sizes:
                print((args.output / f'size{size}-results.json').read_text(), end='')
            return
    else:
        manifest.write_text(json.dumps(metadata, indent=2) + '\n')
    for size in args.sizes:
        started = time.time()
        pairs_per_shard = size_pairs.get(size, args.pairs_per_shard)
        def run(shard):
            seed = args.seed + shard * pairs_per_shard
            log = args.output / f'size{size}-shard{shard}.log'
            done = log.with_suffix('.done.json')
            expected = dict(seed=seed, pairs=pairs_per_shard, size=size, binary_sha256=binary_sha256)
            if done.exists() and json.loads(done.read_text()) != expected:
                raise ValueError(f'Existing shard has different settings or binary: {done}; use a new output directory')
            if not done.exists():
                with log.open('w') as output:
                    subprocess.run([str(binary), f'infbench:{pairs_per_shard}:20:{seed}:static{size}'], stdout=output, stderr=subprocess.STDOUT, check=True)
                done.write_text(json.dumps(expected) + '\n')
            return log
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
            paths = list(pool.map(run, range(args.workers)))
        report = summarize(paths)
        report.update(size=size, pairs=args.workers * pairs_per_shard, concurrent_jobs=args.workers, elapsed_seconds=time.time()-started)
        (args.output / f'size{size}-results.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report), flush=True)
    (args.output / 'complete.json').write_text(json.dumps(dict(completed=time.time()))+'\n')


if __name__ == '__main__':
    main()
