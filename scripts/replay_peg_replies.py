#!/usr/bin/env python3
"""Score one-tile-bag replies with MAGPIE's four-ply sim oracle.

Use staged solver cache logs (default), two-ply infbench logs (--sim2), or
infdecide logs (--control4). Cache models tune on even pairs and score odd
pairs. Control replays score every qualifying position in complete games.
Only public information enters candidate selection. For evaluation, average
over every possible refill given the recorded true leave.
"""
import argparse
import csv
import itertools
import math
import os
import re
from collections import Counter, defaultdict
from pathlib import Path
import statistics
import subprocess
import time

from tune_peg_likelihood import load, posterior


def physical(symbols):
    return ''.join('?' if t.islower() else t for t in symbols)


def control_positions(path):
    positions = {}
    with open(path) as source:
        for line in source:
            if not line.endswith('\n'):
                continue
            if line.startswith('INFDEC_GAME '):
                positions.update(game_positions)
                continue
            if not line.startswith('INFDEC_POS '):
                continue
            row = dict(re.findall(r'(\w+)=(\S+)', line))
            if row['turn'] == '0':
                board = [['.'] * 15 for _ in range(15)]
                scores = [0, 0]
                zeros = 0
                game_positions = {}
            move = row['0_move']
            if move == 'pass' or move.startswith('(exch_'):
                zeros += 1
                continue
            coords, word, points = move.split('_')
            across = coords[0].isdigit()
            col = ord(coords[-1] if across else coords[0]) - ord('A')
            rank = int(coords[:-1] if across else coords[1:]) - 1
            notation = re.sub(r'\(([^)]+)\)', lambda m: '.' * len(m[1]), word)
            letters = physical(notation.replace('.', ''))
            seat = int(row['seat'])
            if 1 <= len(letters) < 7 and int(row['bag']) == 1 + len(letters):
                board_text = '/'.join(re.sub(r'\.+', lambda m: str(len(m[0])), ''.join(r)) for r in board)
                rack = Counter(row['rack'])
                rack.subtract(letters)
                assert min(rack.values()) >= 0
                key = (path, row['game'], row['seed'], row['turn'])
                game_positions[key] = dict(
                    cgp=f"{board_text} {row['rack']}/{row['opp_rack']} {scores[seat]}/{scores[1-seat]} {zeros}",
                    move=f'{coords} {notation}', truth=''.join(sorted(rack.elements())))
            for tile, logged in zip(word.replace('(', '').replace(')', ''), notation):
                expected = tile if logged == '.' else '.'
                assert board[rank][col] == expected
                board[rank][col] = tile
                rank += not across
                col += across
            scores[seat] += int(points)
            zeros = 0 if int(points) else zeros + 1
    return positions


def candidate_leaves(case, distribution):
    board, racks, _, _ = case['cgp'].split()
    pool = distribution.copy()
    observer = racks.split('/')[1]
    for tile in board + observer:
        if tile.isalpha() or tile == '?':
            pool['?' if tile.islower() else tile] -= 1
    symbol = case['move'].split()[1].replace('.', '')
    pool.subtract(physical(symbol))
    assert min(pool.values()) >= 0 and sum(pool.values()) == 8
    leaves = Counter()
    tiles = list(pool.elements())
    for kept in itertools.combinations(tiles, 7 - len(symbol)):
        leave = ''.join(sorted(kept))
        leaves[leave] += 1
    truth = ''.join(sorted(case['truth']))
    assert truth in leaves
    return [dict(leave=leave, ways=count, truth=leave == truth) for leave,count in leaves.items()]


def load_cases(paths, sim2, control4):
    cases = {} if sim2 or control4 else load(paths)
    with open('data/letterdistributions/english.csv') as source:
        distribution = Counter({row[0]: int(row[2]) for row in csv.reader(source)})
    public = {}
    for path in paths:
        if control4:
            public.update(control_positions(path))
            continue
        with open(path) as lines:
            for line in lines:
                if not line.endswith('\n') or not line.startswith(('INFBENCH observation ', 'INFBENCH truth ')):
                    continue
                fields = dict(x.split('=', 1) for x in line.split() if '=' in x)
                key = (path, fields['pair'], fields['game'], fields['turn'])
                if sim2 and (fields['policy'] != 'sim2' or int(key[1]) % 2 == 0):
                    continue
                if not sim2 and key not in cases:
                    continue
                row = public.setdefault(key, {})
                if 'cgp' in fields:
                    row['truth'] = fields['rack']
                    row['cgp'] = line.split(' cgp=')[1].strip()
                else:
                    assert fields['bag'] == '2' and fields['leave'] == '6'
                    row['move'] = line.split(' move=')[1].strip()
    if sim2 or control4:
        cases = {key: candidate_leaves(case, distribution) for key,case in public.items()
                 if 'cgp' in case and 'move' in case}
    return cases, public


def normalize(values):
    mass = sum(values)
    assert mass > 0
    return [x / mass for x in values]


def shortlist(racks, size, floor):
    # The staged solver first eliminates all but K greedy candidates. Use rank
    # as evidence of surviving that stage, with a floor for model mismatch.
    return normalize([r['ways'] * (1 if 0 <= r['rank'] < size else floor)
                      for r in racks])


def search_posteriors(racks, letters, prior, model, sim2):
    # A play consuming m copies of a letter, with k kept, has full-rack
    # multiplicity proportional to the leave multiplicity divided by C(k+m,m).
    full_rack_weights = []
    eligible_weights = []
    sampled_weights = []
    played = Counter(letters)
    for rack in racks:
        leave = rack['leave']
        correction = math.prod(math.comb(leave.count(tile) + count, count)
                               for tile, count in played.items())
        weight = rack['ways'] / correction
        rank, likelihood = model[leave]
        full_rack_weights.append(weight)
        eligible_weights.append(weight if rank >= 0 else 0)
        sampled_weights.append(weight * likelihood)
    assert len(model) == len(racks)
    weights_by_policy = {
        'rack_prior': full_rack_weights,
        'top15' if sim2 else 'top100': eligible_weights,
        'sim2x8' if sim2 else 'sim4x8': sampled_weights,
    }
    for name, weights in weights_by_policy.items():
        posterior_weights = normalize(weights) if any(weights) else prior
        yield name, [.99 * p + .01 * q for p, q in zip(posterior_weights, prior)]


def project(tiles, racks, weights):
    bag_size = sum(tiles.values()) - len(racks[0]['leave'])
    return {t: sum(p * (copies - r['leave'].count(t)) / bag_size
                   for r, p in zip(racks, weights)) for t, copies in tiles.items()}


def choose(values, weights):
    # All policies see the same list in static order. Break numerical ties in
    # that order. The engine's objective here is expected blended utility.
    return max(range(len(next(iter(values.values())))), key=lambda idx:
               round(sum(p * values[t][idx][0] for t, p in weights.items()), 11))


def read_matrix(pipe):
    values, static, tiles = {}, {}, {}
    model, seconds = {}, 0.0
    for line in pipe:
        if line.startswith('REPLY_TILE '):
            _, tile, copies, prob, *moves = line.split()
            assert tile not in tiles
            tiles[tile] = int(copies)
            static[tile] = float(prob)
            values[tile] = [tuple(map(float, mv.split(','))) for mv in moves]
        elif line.startswith('REPLY_LIKELIHOOD '):
            _, leave, rank, likelihood = line.split()
            model[leave] = (int(rank), float(likelihood))
        elif line.startswith('REPLY_MODEL_SECONDS '):
            seconds = tuple(map(float, line.split()[1:]))
        elif line.startswith('REPLY_END'):
            break
    else:
        raise RuntimeError('infreply exited before completing a position')
    assert sum(tiles.values()) == 8 and abs(sum(static.values()) - 1) < 1e-8
    assert min(static.values()) >= -1e-9
    assert len({len(v) for v in values.values()}) == 1
    return values, static, tiles, model, seconds


def clustered(values):
    n = len(values)
    mean = statistics.mean(v for _, v in values)
    residuals = defaultdict(float)
    for pair, value in values:
        residuals[pair] += value - mean
    g = len(residuals)
    se = math.sqrt(g / (g - 1) * sum(x*x for x in residuals.values())) / n if g > 1 else math.nan
    return mean, 1.96 * se


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    opponents = parser.add_mutually_exclusive_group()
    opponents.add_argument('--control4', action='store_true', help='reconstruct one-tile-bag replies from infdecide logs')
    opponents.add_argument('--sim2', action='store_true', help='model the 15-candidate two-ply sim from sim2 logs')
    parser.add_argument('--per-worker', type=int, help='first N odd-pair positions per worker')
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--csv', type=Path, help='save one scored row per decision and policy')
    parser.add_argument('--limit', type=int, help='first N odd-pair positions, for a smoke test')
    parser.add_argument('--idle-factor', type=float, default=2.0,
                        help='idle this multiple of elapsed work time between positions')
    parser.add_argument('paths', nargs='+')
    args = parser.parse_args()
    cases, public = load_cases(args.paths, args.sim2, args.control4)
    engine_model = args.sim2 or args.control4
    assert args.control4 or len({Path(p).name.split('-')[0] for p in args.paths}) == 1
    valid = list(cases)
    print(f'{len(valid)} complete positions in input', flush=True)
    train = [cases[k] for k in valid if int(k[1]) % 2 == 0]

    def train_score(param):
        size, floor = param
        return sum(math.log(sum(p for r, p in zip(racks, shortlist(racks, size, floor)) if r['truth']))
                   for racks in train)
    if not engine_model:
        best = max(((size, floor) for size in (1, 2, 4, 8, 16, 32)
                for floor in (.001, .01, .05, .2)), key=train_score)
        print(f'even-pair shortlist K={best[0]} floor={best[1]}', flush=True)
    rows = []
    test = [k for k in valid if args.control4 or int(k[1]) % 2]
    if args.per_worker:
        test = [key for path in args.paths for key in [k for k in test if k[0] == path][:args.per_worker]]
    if not test or args.limit == 0:
        parser.error('empty test set')
    model_times = []
    environment = dict(os.environ)
    environment.pop('INFREPLY_SIM2', None)
    if args.sim2:
        environment['INFREPLY_SIM2'] = '1'
    with subprocess.Popen([str(args.binary.resolve()), 'infreply'], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, text=True, bufsize=1, env=environment) as engine:
        try:
            for key in test[:args.limit]:
                start = time.monotonic()
                racks = cases[key]
                case = public[key]
                board, hidden, score, zeros = case['cgp'].split()
                played = case['move'].split()[1].replace('.', '')
                letters = physical(played)
                observer = hidden.split('/')[1]
                feasible = min(r['leave'] for r in racks) + letters
                suffix = '\t' + ','.join(r['leave'] for r in racks) if engine_model else ''
                engine.stdin.write(f"{board} {feasible}/{observer} {score} {zeros}\t{case['move']}{suffix}\n")
                engine.stdin.flush()
                values, static, tiles, model, seconds = read_matrix(engine.stdout)
                for rack in racks:
                    counts = Counter(rack['leave'])
                    assert sum(counts.values()) == 7 - len(letters) and all(v <= tiles[t] for t,v in counts.items())
                    assert rack['ways'] == math.prod(math.comb(tiles[t], v) for t,v in counts.items())
                truth = next(i for i, r in enumerate(racks) if r['truth'])
                truth_bag = project(tiles, [racks[truth]], [1.0])
                prior = normalize([r['ways'] for r in racks])
                prior_bag = project(tiles, racks, prior)
                assert all(abs(prior_bag[t] - n/8) < 1e-10 for t,n in tiles.items())
                policies = {'T8': static}
                supported = None
                if engine_model:
                    model_times.append(seconds)
                    supported = model[racks[truth]['leave']][0] >= 0
                    for name, post in search_posteriors(racks, letters, prior, model, args.sim2):
                        policies[name] = project(tiles, racks, post)
                else:
                    soft, prior = posterior(racks, 200, 8)
                    soft = [.99 * p + .01 * q for p, q in zip(soft, prior)]
                    policies['solver'] = project(tiles, racks, soft)
                    policies['shortlist'] = project(tiles, racks, shortlist(racks, *best))
                policies['known_leave'] = truth_bag
                control = choose(values, prior_bag)
                n_moves = len(next(iter(values.values())))
                win_by_move = [sum(p * values[t][idx][1] for t,p in truth_bag.items())
                               for idx in range(n_moves)]
                win_headroom = 100 * (max(win_by_move) - win_by_move[control])
                for name, prob in policies.items():
                    picked = choose(values, prob)
                    delta = [sum(p * (values[t][picked][i] - values[t][control][i])
                                 for t,p in truth_bag.items()) for i in range(3)]
                    # This is a score for the unknown final bag tile, hence
                    # also for the full rack that the responding player faces.
                    gain = sum(p * math.log(max(prob[t], 1e-300) / prior_bag[t])
                               for t,p in truth_bag.items() if p)
                    rows.append(dict(worker=key[2] if args.control4 else Path(key[0]).stem, pair=key[1], game=key[2], turn=key[3],
                                     policy=name, truth_supported=supported, control_idx=control, policy_idx=picked,
                                     win_headroom=win_headroom, changed=int(picked != control), bag_gain=gain,
                                     win=100*delta[1], utility=100*delta[0], equity=delta[2]))
                if args.idle_factor > 0:
                    time.sleep((time.monotonic() - start) * args.idle_factor)
        finally:
            engine.stdin.close()
            if engine.poll() is None and not rows:
                engine.terminate()
        if engine.wait():
            raise RuntimeError('reply engine failed')
    if args.csv:
        with args.csv.open('w', newline='') as out:
            writer = csv.DictWriter(out, fieldnames=rows[0])
            writer.writeheader()
            writer.writerows(rows)
    headroom = [r['win_headroom'] for r in rows if r['policy'] == 'known_leave']
    print(f'{sum(x > 1e-8 for x in headroom)}/{len(headroom)} positions allow a win gain over the '
          f'prior choice given the true leave, mean maximum {statistics.mean(headroom):.3f} pp')
    if model_times:
        for label, timings in zip(('candidate generation', 'whole opponent model'), zip(*model_times)):
            timings = sorted(timings)
            print(f'{label} wall seconds median {statistics.median(timings):.4f}, '
                  f'p95 {timings[math.ceil(.95*len(timings))-1]:.4f}, max {max(timings):.4f}')
        checks = [r['truth_supported'] for r in rows if r['policy'] == 'T8']
        print(f'observed move in actual rack shortlist {sum(checks)}/{len(checks)}')
    print('policy        n   changed%   bag gain(nats)   win pp   utility pp   equity pts (95% half widths)')
    for name in policies:
        sample = [r for r in rows if r['policy'] == name]
        print(f"{name:12s} {len(sample):4} {100*statistics.mean(r['changed'] for r in sample):8.2f}", end='')
        for field in ('bag_gain','win','utility','equity'):
            mean, half = clustered([((r['worker'], r['pair']), r[field]) for r in sample])
            print(f'   {mean:+.3f} ±{half:.3f}', end='')
        print()


if __name__ == '__main__':
    main()
