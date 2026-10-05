#!/usr/bin/env python3
"""Split, fit and evaluate per-lexicon blocking/setup parameters.

Inputs come from the bsgen stages of magpie_test (see
test/blocking_setup_gen_test.c): positions.csv, labels*.csv and refs*.csv.
Everything here is standard-library Python so it runs anywhere.

Subcommands
  split     Freeze a train/validation/test split by game (one position per
            game, so related positions stay together). Writes a manifest with
            input hashes. Run once, before any tuning.
  teacher   Teacher precision: how well cheaper rack counts reproduce the
            largest one (rank agreement, admission agreement).
  fit       Grid-search weights on the training games for one objective and
            report them on the validation games. Writes a .bsp file.
  choices   The candidates any grid weight would pick under static_choice,
            per game ("game,cand" lines for bsgen refs cands=). Uses labels
            only, never outcomes, so references can concentrate on them.
  evaluate  Score fixed weights on one split; --split test needs --final.
  volatility  On train/validation only: does a fixed policy's gain vary with
            score lead, bag phase, PAT's pre-board value, the spread of the
            candidates' deltas, or extreme candidates?

Objectives (the units differ and the weights are not interchangeable):
  static_choice  Pick argmax(static_eq + wb * blocking + ws * setup) over the
                 universe; gain = reference WP of that pick minus reference
                 WP of the plain static pick. Selection never reads the
                 reference, so one reference gives an unbiased estimate.
  sim_admission  Admit static top-N plus the top-N by static_eq + wb *
                 blocking and by static_eq + ws * setup (plus exchanges);
                 gain = reference WP of the best admitted play minus that of
                 the best play among the same number of static plays. The
                 "best" is chosen with reference A and valued with an
                 independent reference B when --refs-b is given; with one
                 reference the max is biased upward (reported as such).
WP is in percentage points. Deltas and weights are in score points (a
weight is equity points per delta point).
"""

import argparse
import csv
import hashlib
import json
import math
import os
import random
import statistics
import sys
from collections import defaultdict


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def read_rows(paths):
    rows = []
    for path in paths:
        with open(path, newline='') as handle:
            rows.extend(csv.DictReader(handle))
    return rows


def load_positions(labels_paths, refs_paths, rack_key):
    """Positions keyed by game id, each with its candidate list."""
    positions = {}
    for row in read_rows(labels_paths):
        game = int(row['game'])
        position = positions.setdefault(game, {
            'game': game, 'bag': int(row['bag']), 'lead': int(row['lead']),
            'phase': row['phase'], 'pass_pat_term': float(row['pass_pat_term']),
            'pass_reply': float(row['pass_reply']), 'cands': {}})
        cand = int(row['cand'])
        entry = {
            'cand': cand, 'move': row['move'], 'source': row['source'],
            'static_rank': int(row['static_rank']),
            'pat_rank': int(row['pat_rank']), 'type': row['type'],
            'static_eq': float(row['static_eq']),
            'pat_eq': float(row['pat_eq']),
            'deltas': {}}
        for key, value in row.items():
            if key.startswith('blocking_r') or key.startswith('setup_r'):
                entry['deltas'][key] = float(value)
        entry['blocking'] = entry['deltas']['blocking_' + rack_key]
        entry['setup'] = entry['deltas']['setup_' + rack_key]
        position['cands'][cand] = entry
    for label, paths in refs_paths.items():
        if not paths:
            continue
        for row in read_rows(paths):
            game = int(row['game'])
            if game not in positions:
                continue
            cand = positions[game]['cands'].get(int(row['cand']))
            if cand is None or cand['move'] != row['move']:
                raise SystemExit(f'reference row does not match labels: {row}')
            cand['wp_' + label] = 100.0 * float(row['sim_wp'])
            cand['sem_' + label] = 100.0 * float(row['sim_wp_sem'])
    return positions


def complete(position, labels, needed=None):
    cands = position['cands'].values() if needed is None else needed
    return all(all('wp_' + label in cand for label in labels)
               for cand in cands)


def ranked(cands, key):
    """Candidates by key descending; ties by static rank (deterministic)."""
    return sorted(cands, key=lambda cand: (-key(cand), cand['static_rank']))


def static_order(position):
    return sorted(position['cands'].values(), key=lambda c: c['static_rank'])


def adjusted(cand, blocking_weight, setup_weight):
    return (cand['static_eq'] + blocking_weight * cand['blocking'] +
            setup_weight * cand['setup'])


def static_pick_for(position, blocking_weight, setup_weight):
    cands = list(position['cands'].values())
    return ranked(cands, lambda c: adjusted(c, blocking_weight,
                                            setup_weight))[0]


def static_choice_gain(position, blocking_weight, setup_weight, ref='A'):
    static_pick = static_order(position)[0]
    pick = static_pick_for(position, blocking_weight, setup_weight)
    if pick is static_pick:
        return 0.0, False
    return pick['wp_' + ref] - static_pick['wp_' + ref], True


def admission_pool(position, blocking_weight, setup_weight, args):
    cands = static_order(position)
    pool = {c['cand'] for c in cands[:args.static_nominees]}
    if blocking_weight is not None:
        pool |= {c['cand'] for c in ranked(
            cands, lambda c: c['static_eq'] + blocking_weight *
            c['blocking'])[:args.check_nominees]}
    if setup_weight is not None:
        pool |= {c['cand'] for c in ranked(
            cands, lambda c: c['static_eq'] + setup_weight *
            c['setup'])[:args.check_nominees]}
    best = cands[0]['static_eq']
    exchanges = [c for c in cands if c['type'] == 'exchange' and
                 c['static_eq'] >= best - args.exchange_margin]
    pool |= {c['cand'] for c in exchanges[:args.exchange_quota]}
    return pool


def admission_gain(position, blocking_weight, setup_weight, args, refs):
    pool = admission_pool(position, blocking_weight, setup_weight, args)
    static = {c['cand'] for c in static_order(position)[:len(pool)]}
    select, value = refs
    cands = position['cands']

    def best_value(cand_ids):
        chosen = max(cand_ids, key=lambda cand: (cands[cand]['wp_' + select],
                                                 -cands[cand]['static_rank']))
        return cands[chosen]['wp_' + value]
    return best_value(pool) - best_value(static), pool != static


def summarize(gains):
    if not gains:
        return {'n': 0}
    mean = statistics.fmean(gains)
    sd = statistics.pstdev(gains) if len(gains) > 1 else 0.0
    se = sd / math.sqrt(len(gains)) if len(gains) > 1 else float('nan')
    return {'n': len(gains), 'mean_pp': mean, 'se_pp': se,
            't': mean / se if se and se > 0 else float('nan')}


def bootstrap(gains, seed, reps=2000):
    if len(gains) < 2:
        return [float('nan'), float('nan')]
    rng = random.Random(seed)
    means = sorted(statistics.fmean(rng.choices(gains, k=len(gains)))
                   for _ in range(reps))
    return [means[int(0.025 * reps)], means[int(0.975 * reps) - 1]]


def load_split(path):
    with open(path) as handle:
        return json.load(handle)


def games_in(split, name):
    return {int(game) for game, assigned in split['assignment'].items()
            if assigned == name}


def cmd_split(args):
    if os.path.exists(args.out):
        raise SystemExit(f'{args.out} exists; a split is frozen once made')
    games = sorted({int(row['game']) for row in read_rows(args.positions)})
    assignment = {}
    for game in games:
        digest = hashlib.sha256(f'{args.seed}:{args.lexicon}:{game}'.encode())
        unit = int(digest.hexdigest()[:12], 16) / float(1 << 48)
        if unit < args.train:
            assignment[game] = 'train'
        elif unit < args.train + args.validation:
            assignment[game] = 'validation'
        else:
            assignment[game] = 'test'
    manifest = {
        'lexicon': args.lexicon, 'seed': args.seed,
        'fractions': {'train': args.train, 'validation': args.validation},
        'unit': 'game (one position per game)',
        'positions_sha256': {path: sha256_file(path)
                             for path in args.positions},
        'counts': {name: sum(1 for v in assignment.values() if v == name)
                   for name in ('train', 'validation', 'test')},
        'assignment': {str(game): name for game, name in assignment.items()},
    }
    with open(args.out, 'w') as handle:
        json.dump(manifest, handle, indent=1)
    print(json.dumps(manifest['counts']))


def spearman(xs, ys):
    def ranks(values):
        order = sorted(range(len(values)), key=lambda i: values[i])
        result = [0.0] * len(values)
        idx = 0
        while idx < len(order):
            end = idx
            while end + 1 < len(order) and values[order[end + 1]] == \
                    values[order[idx]]:
                end += 1
            for k in range(idx, end + 1):
                result[order[k]] = (idx + end) / 2.0
            idx = end + 1
        return result
    rx, ry = ranks(xs), ranks(ys)
    if len(xs) < 3 or statistics.pstdev(rx) == 0 or statistics.pstdev(ry) == 0:
        return float('nan')
    return statistics.correlation(rx, ry)


def cmd_teacher(args):
    positions = load_positions(args.labels, {}, args.racks)
    keys = sorted({key[len('blocking_r'):] for pos in positions.values()
                   for cand in pos['cands'].values()
                   for key in cand['deltas'] if key.startswith('blocking_r')},
                  key=int)
    top = keys[-1]
    report = {}
    for key in keys[:-1]:
        for kind, weight in (('blocking', 1.4), ('setup', 0.75)):
            rhos, agree, moved = [], 0, []
            for pos in positions.values():
                cands = list(pos['cands'].values())
                xs = [c['deltas'][f'{kind}_r{key}'] for c in cands]
                ys = [c['deltas'][f'{kind}_r{top}'] for c in cands]
                rho = spearman(xs, ys)
                if not math.isnan(rho):
                    rhos.append(rho)

                def admit(rack):
                    return {c['cand'] for c in ranked(
                        cands, lambda c: c['static_eq'] + weight *
                        c['deltas'][f'{kind}_r{rack}'])[:25]}
                cheap, full = admit(key), admit(top)
                agree += cheap == full
                moved.append(len(full - cheap))
            report[f'{kind}_r{key}_vs_r{top}'] = {
                'median_spearman': statistics.median(rhos) if rhos else None,
                'top25_admission_identical': agree / len(positions),
                'mean_top25_admissions_replaced': statistics.fmean(moved)}
    print(json.dumps(report, indent=1))


def grid(text):
    return [float(value) for value in text.split(',')]


def objective_gains(args, positions, games, blocking_weight, setup_weight,
                    refs):
    gains = []
    changed = 0
    for game in games:
        position = positions[game]
        if args.objective == 'static_choice':
            gain, differs = static_choice_gain(position, blocking_weight,
                                               setup_weight, refs[1])
        else:
            gain, differs = admission_gain(position, blocking_weight,
                                           setup_weight, args, refs)
        gains.append(gain)
        changed += differs
    return gains, changed


def grid_choices(position, args):
    picks = {static_order(position)[0]['cand']}
    for blocking_weight in grid(args.blocking_grid):
        for setup_weight in grid(args.setup_grid):
            picks.add(static_pick_for(position, blocking_weight,
                                      setup_weight)['cand'])
    return picks


def prepared(args):
    refs_paths = {'A': args.refs, 'B': args.refs_b or []}
    positions = load_positions(args.labels, refs_paths, args.racks)
    labels = ['A'] + (['B'] if args.refs_b else [])
    refs = ('A', 'B') if args.refs_b else ('A', 'A')
    split = load_split(args.split_file)
    usable = set()
    for game, pos in positions.items():
        needed = None
        if args.objective == 'static_choice':
            # Only the grid's picks need references (see choices).
            needed = [pos['cands'][cand] for cand in grid_choices(pos, args)]
            if len(needed) == 1:
                usable.add(game)
                continue
        if complete(pos, labels, needed):
            usable.add(game)
    missing = len(positions) - len(usable)
    if missing:
        print(f'note: {missing} positions lack references and are skipped',
              file=sys.stderr)
    return positions, split, usable, refs


def cmd_choices(args):
    positions = load_positions(args.labels, {}, args.racks)
    with open(args.out, 'w') as handle:
        handle.write('game,cand\n')
        for game in sorted(positions):
            for cand in sorted(grid_choices(positions[game], args)):
                handle.write(f'{game},{cand}\n')


def cmd_fit(args):
    positions, split, usable, refs = prepared(args)
    train = sorted(games_in(split, 'train') & usable)
    validation = sorted(games_in(split, 'validation') & usable)
    if not train:
        raise SystemExit('no complete training positions')
    best = None
    table = []
    for blocking_weight in grid(args.blocking_grid):
        for setup_weight in grid(args.setup_grid):
            gains, changed = objective_gains(args, positions, train,
                                             blocking_weight, setup_weight,
                                             refs)
            mean = statistics.fmean(gains)
            table.append((blocking_weight, setup_weight, mean, changed))
            if best is None or mean > best[2]:
                best = (blocking_weight, setup_weight, mean, changed)
    blocking_weight, setup_weight = best[0], best[1]
    val_gains, val_changed = objective_gains(args, positions, validation,
                                             blocking_weight, setup_weight,
                                             refs)
    result = {
        'objective': args.objective, 'racks': args.racks,
        'reference_selection': refs[0], 'reference_value': refs[1],
        'biased_max': args.objective == 'sim_admission' and refs[0] == refs[1],
        'train': {'n': len(train), 'best_blocking_weight': blocking_weight,
                  'best_setup_weight': setup_weight, 'mean_pp': best[2],
                  'changed': best[3]},
        'validation': dict(summarize(val_gains), changed=val_changed,
                           bootstrap95=bootstrap(val_gains, args.seed)),
        'grid': [{'blocking_weight': b, 'setup_weight': s, 'train_mean_pp': m,
                  'changed': c} for b, s, m, c in table],
    }
    print(json.dumps({k: v for k, v in result.items() if k != 'grid'},
                     indent=1))
    if args.report:
        with open(args.report, 'w') as handle:
            json.dump(result, handle, indent=1)
    if args.out:
        write_bsp(args, blocking_weight, setup_weight, result)


def write_bsp(args, blocking_weight, setup_weight, result):
    inputs = {os.path.basename(p): sha256_file(p)
              for p in args.labels + args.refs + (args.refs_b or [])}
    lines = [
        'magpie_bsp_v1',
        f'# Fitted by test/blocking_setup/bs_fit.py ({args.objective}); '
        'deltas in points, weights in equity points per delta point.',
        f'lexicon,{args.lexicon}',
        f'model_version,{args.model_version}',
        f'objective,{args.objective}',
        f'teacher_racks,{args.racks[1:]}',
        'teacher_partition,1',
        'teacher_condition_draws,1',
        'teacher_followup_draws,1',
        f'blocking_weight,{blocking_weight:.6f}',
        f'setup_weight,{setup_weight:.6f}',
        f'provenance,split={os.path.basename(args.split_file)} '
        f'sha256={sha256_file(args.split_file)}',
        f'provenance,train_n={result["train"]["n"]} '
        f'validation_n={result["validation"]["n"]} '
        f'validation_mean_pp={result["validation"].get("mean_pp", 0):.6f}',
    ]
    lines += [f'provenance,input {name} sha256={digest}'
              for name, digest in sorted(inputs.items())]
    with open(args.out, 'w') as handle:
        handle.write('\n'.join(lines) + '\n')


def cmd_evaluate(args):
    positions, split, usable, refs = prepared(args)
    if args.split == 'test' and not args.final:
        raise SystemExit('the test split is the frozen holdout; pass --final '
                         'only for the one final evaluation')
    games = sorted(games_in(split, args.split) & usable)
    gains, changed = objective_gains(args, positions, games,
                                     args.blocking_weight, args.setup_weight,
                                     refs)
    by_phase = defaultdict(list)
    for game, gain in zip(games, gains):
        by_phase[positions[game]['phase']].append(gain)
    print(json.dumps({
        'split': args.split, 'objective': args.objective,
        'weights': [args.blocking_weight, args.setup_weight],
        'overall': dict(summarize(gains), changed=changed,
                        bootstrap95=bootstrap(gains, args.seed)),
        'by_phase': {phase: summarize(values)
                     for phase, values in sorted(by_phase.items())},
    }, indent=1))


def volatility_features(position):
    cands = static_order(position)
    top = cands[:25]
    blocking = [c['blocking'] for c in top]
    setup = [c['setup'] for c in top]
    pat_rel = [c['pat_eq'] - c['static_eq'] - position['pass_pat_term']
               for c in top]
    return {
        'lead': position['lead'],
        'bag': position['bag'],
        'pass_reply': position['pass_reply'],
        'pat_board': position['pass_pat_term'],
        'blocking_sd': statistics.pstdev(blocking),
        'blocking_max': max(blocking),
        'blocking_min': min(blocking),
        'setup_sd': statistics.pstdev(setup),
        'pat_rel_sd': statistics.pstdev(pat_rel),
        'pat_rel_min': min(pat_rel),
        'static_gap': cands[0]['static_eq'] - cands[min(4, len(cands) - 1)][
            'static_eq'],
    }


def cmd_volatility(args):
    positions, split, usable, refs = prepared(args)
    games = sorted((games_in(split, 'train') | games_in(split, 'validation'))
                   & usable)
    gains, _ = objective_gains(args, positions, games, args.blocking_weight,
                               args.setup_weight, refs)
    features = [volatility_features(positions[game]) for game in games]
    report = {}
    for name in features[0]:
        values = [feature[name] for feature in features]
        order = sorted(range(len(games)), key=lambda i: values[i])
        quartiles = []
        for quarter in range(4):
            members = order[quarter * len(order) // 4:(quarter + 1) *
                            len(order) // 4]
            quartiles.append({
                'range': [values[members[0]], values[members[-1]]]
                if members else None,
                **summarize([gains[i] for i in members])})
        report[name] = {'spearman_with_gain': spearman(values, gains),
                        'quartiles': quartiles}
    print(json.dumps({'objective': args.objective,
                      'weights': [args.blocking_weight, args.setup_weight],
                      'n': len(games), 'features': report}, indent=1))


def add_grids(sub_parser):
    sub_parser.add_argument('--blocking-grid',
                            default='0,0.25,0.5,0.75,1,1.4,2,3')
    sub_parser.add_argument('--setup-grid', default='0,0.25,0.5,0.75,1,1.5')


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawTextHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)

    split = sub.add_parser('split')
    split.add_argument('--positions', nargs='+', required=True)
    split.add_argument('--lexicon', required=True)
    split.add_argument('--seed', type=int, required=True)
    split.add_argument('--train', type=float, default=0.6)
    split.add_argument('--validation', type=float, default=0.2)
    split.add_argument('--out', required=True)
    split.set_defaults(func=cmd_split)

    teacher = sub.add_parser('teacher')
    teacher.add_argument('--labels', nargs='+', required=True)
    teacher.add_argument('--racks', default='r64')
    teacher.set_defaults(func=cmd_teacher)

    def common(sub_parser):
        sub_parser.add_argument('--labels', nargs='+', required=True)
        sub_parser.add_argument('--refs', nargs='+', required=True)
        sub_parser.add_argument('--refs-b', nargs='+')
        sub_parser.add_argument('--split-file', required=True)
        sub_parser.add_argument('--racks', default='r64')
        sub_parser.add_argument('--objective', default='static_choice',
                                choices=['static_choice', 'sim_admission'])
        sub_parser.add_argument('--static-nominees', type=int, default=25)
        sub_parser.add_argument('--check-nominees', type=int, default=25)
        sub_parser.add_argument('--exchange-quota', type=int, default=5)
        sub_parser.add_argument('--exchange-margin', type=float, default=35.0)
        sub_parser.add_argument('--seed', type=int, default=1)
        add_grids(sub_parser)

    fit = sub.add_parser('fit')
    common(fit)
    fit.add_argument('--lexicon', required=True)
    fit.add_argument('--model-version', required=True)
    fit.add_argument('--out')
    fit.add_argument('--report')
    fit.set_defaults(func=cmd_fit)

    choices = sub.add_parser('choices')
    choices.add_argument('--labels', nargs='+', required=True)
    choices.add_argument('--racks', default='r64')
    choices.add_argument('--out', required=True)
    add_grids(choices)
    choices.set_defaults(func=cmd_choices)

    evaluate = sub.add_parser('evaluate')
    common(evaluate)
    evaluate.add_argument('--split', default='validation',
                          choices=['train', 'validation', 'test'])
    evaluate.add_argument('--final', action='store_true')
    evaluate.add_argument('--blocking-weight', type=float, required=True)
    evaluate.add_argument('--setup-weight', type=float, required=True)
    evaluate.set_defaults(func=cmd_evaluate)

    volatility = sub.add_parser('volatility')
    common(volatility)
    volatility.add_argument('--blocking-weight', type=float, required=True)
    volatility.add_argument('--setup-weight', type=float, required=True)
    volatility.set_defaults(func=cmd_volatility)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == '__main__':
    main(sys.argv[1:])
