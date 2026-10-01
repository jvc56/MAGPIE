#!/usr/bin/env python3
"""Build an offline candidate-pool explorer from a pools_study.sh run.

  build_pool_explorer.py <out_dir> <positions.csv> <data_dir> <explorer.html>
      [--title TEXT] [--distribution english] [--layout standard15]

data_dir is MAGPIE's data directory (letter distribution and layout). The
page lists every candidate of every pool with its provenance, static rank,
score, leave, static equity, blocking/setup adjustments and check means,
each pool's selection-sim results and the reference, on a board with
classic premium colors, tile scores and blank styling, plus the scores,
racks and unseen tiles. It checks pool counts, choices and that every
placement agrees with the board before writing.
"""
import argparse
import csv
import glob
import json
import os
import re
from collections import Counter, defaultdict

SOURCE_BITS = [(1, 'static'), (2, 'pat'), (4, 'blocking'), (8, 'setup'),
               (16, 'exchange')]


def rows(pattern):
    result = []
    for path in sorted(glob.glob(pattern)):
        with open(path, newline='') as handle:
            result.extend(csv.DictReader(handle))
    return result


def parse_board(cgp):
    result = []
    for encoded in cgp.split()[0].split('/'):
        line = []
        for token in re.findall(r'\d+|.', encoded):
            line.extend([''] * int(token) if token.isdigit() else [token])
        assert len(line) == 15, encoded
        result.append(line)
    assert len(result) == 15
    return result


def check_placement(position, candidate):
    coordinate, word = candidate['move'].split()
    vertical = coordinate[0].isalpha()
    column = ord(re.search('[a-o]', coordinate).group()) - ord('a')
    row_index = int(re.search(r'\d+', coordinate).group()) - 1
    added = 0
    for offset, letter in enumerate(word):
        row = row_index + (offset if vertical else 0)
        col = column + (0 if vertical else offset)
        assert 0 <= row < 15 and 0 <= col < 15, candidate['move']
        existing = position['board'][row][col]
        assert not existing or existing == letter or letter == '.', (
            position['id'], candidate['move'])
        added += not bool(existing)
    assert added == candidate['tiles'], (position['id'], candidate['move'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out_dir')
    parser.add_argument('positions')
    parser.add_argument('data_dir')
    parser.add_argument('output')
    parser.add_argument('--title', default='Pool comparison')
    parser.add_argument('--distribution', default='english')
    parser.add_argument('--layout', default='standard15')
    parser.add_argument('--plies', type=int, default=4)
    args = parser.parse_args()
    with open(os.path.join(args.data_dir, 'letterdistributions',
                           args.distribution + '.csv'), newline='') as handle:
        distribution_rows = list(csv.reader(handle))
    tile_scores = {row[0]: int(row[3]) for row in distribution_rows}
    tile_counts = {row[0]: int(row[2]) for row in distribution_rows}
    with open(os.path.join(args.data_dir, 'layouts',
                           args.layout + '.txt')) as handle:
        premiums = [list(line) for line in handle.read().splitlines()[1:16]]
    positions = {}
    with open(args.positions, newline='') as handle:
        for row in csv.DictReader(handle):
            game = int(row['game'])
            fields = row['cgp'].split()
            positions[game] = dict(
                id=game, cgp=row['cgp'], board=parse_board(row['cgp']),
                racks=fields[1].split('/'),
                scores=list(map(int, fields[2].split('/'))), onturn=0,
                meta={'game': game, 'turn': int(row['turn'])},
                bag=int(row['bag']), candidates={}, pools={})
    pools_rows = rows(os.path.join(args.out_dir, 'w*.pools.csv'))
    arm_order = []
    for row in pools_rows:
        if row['pool'] not in arm_order:
            arm_order.append(row['pool'])
    # The first equal-size static pool anchors the provenance groups.
    statics = [arm for arm in arm_order if arm.startswith('static_')]
    arms = statics[:1] + [arm for arm in arm_order if arm not in statics[:1]]
    used = {int(row['game']) for row in pools_rows}
    positions = {game: pos for game, pos in positions.items() if game in used}

    def candidate(position, row):
        entry = position['candidates'].setdefault(row['move'], dict(
            move=row['move'], rank=None, score=None, eq=None, leave=None,
            tiles=None, type=1 if row.get('type', 'place') == 'place' else 5,
            results={}, sources=set()))
        return entry

    for row in rows(os.path.join(args.out_dir, 'w*.nominees.csv')):
        position = positions[int(row['game'])]
        entry = candidate(position, row)
        entry.update(rank=int(row['static_rank']), score=int(row['score']),
                     eq=float(row['static_eq']), leave=row['leave'],
                     tiles=int(row['tiles_played']))
        sources = int(row['sources'])
        entry['sources'] |= {name for bit, name in SOURCE_BITS if sources & bit}
        if row['checked'] == '1':
            entry['check'] = {
                'blocking_adjustment': float(row['blocking_adj']),
                'setup_adjustment': float(row['setup_adj']),
                'pass_reply_mean': float(row['pass_reply']),
                'candidate_reply_mean': float(row['cand_reply']),
                'pass_followup_mean': float(row['pass_followup']),
                'candidate_followup_mean': float(row['cand_followup'])}
    for row in pools_rows:
        position = positions[int(row['game'])]
        position['pools'][row['pool']] = dict(
            count=int(row['count']),
            seconds=round(float(row['wall_ms']) / 1000, 2),
            reused=row['same_as'] or None)
    select = defaultdict(dict)
    for row in rows(os.path.join(args.out_dir, 'w*.select.csv')):
        select[int(row['game']), row['pool']][row['move']] = [
            float(row['sim_wp']), float(row['sim_wp_sem']),
            float(row['sim_eq']), float(row['sim_eq_sem']), int(row['chosen'])]
    for row in pools_rows:
        game, pool = int(row['game']), row['pool']
        source_pool = row['same_as'] or pool
        results = select.get((game, source_pool))
        position = positions[game]
        if results is None:
            # A one-candidate pool needs no sim.
            assert int(row['count']) == 1, (game, pool)
            entry = position['candidates'][row['chosen']]
            entry['results'][pool] = [None, None, None, None, 1]
            continue
        assert len(results) == int(row['count']), (game, pool)
        for move, values in results.items():
            position['candidates'][move]['results'][pool] = values
        assert results[row['chosen']][4] == 1
    refs = defaultdict(dict)
    for row in rows(os.path.join(args.out_dir, 'w*.refs.csv')):
        refs[int(row['game'])][row['move']] = row
    for game, position in positions.items():
        best = max(refs[game].values(), key=lambda r: float(r['ref_wp']))
        for move, row in refs[game].items():
            position['candidates'][move]['results']['reference'] = [
                float(row['ref_wp']), float(row['ref_wp_sem']),
                float(row['ref_eq']), float(row['ref_eq_sem']),
                int(row is best)]
        position['pools']['reference'] = dict(
            count=len(refs[game]), seconds=None, reused=None)
        for entry in position['candidates'].values():
            assert entry['rank'] is not None, (game, entry['move'])
            if entry['type'] == 1:
                check_placement(position, entry)
            entry['sources'] = sorted(entry['sources'])
        for arm in arms + ['reference']:
            members = [c for c in position['candidates'].values()
                       if arm in c['results']]
            assert len(members) == position['pools'][arm]['count'], (game, arm)
            assert sum(c['results'][arm][4] for c in members) == 1, (game, arm)
        unseen = Counter(tile_counts)
        for line in position['board']:
            for letter in line:
                if letter:
                    unseen['?' if letter.islower() else letter] -= 1
        unseen.subtract(position['racks'][0])
        assert all(count >= 0 for count in unseen.values())
        position['unseen'] = dict(unseen)
        position['candidates'] = sorted(position['candidates'].values(),
                                        key=lambda c: c['rank'])
    differing = [game for game, position in sorted(positions.items())
                 if len({next(c['move'] for c in position['candidates']
                              if c['results'].get(arm, [0] * 5)[4])
                         for arm in arms}) > 1]
    examples = ''.join(f'<button class="jump" data-id="{game}">{game}</button>'
                       for game in differing[:8])
    data = dict(datasets=[dict(
        name='pools', title=args.title, arms=arms, plies=args.plies,
        examples=examples,
        positions=[positions[game] for game in sorted(positions)])],
        premiums=premiums, tileScores=tile_scores)
    template_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 'pool_explorer_template.html')
    with open(template_path) as handle:
        template = handle.read()
    with open(args.output, 'w') as handle:
        handle.write(template.replace('/*DATA*/null',
                                      json.dumps(data, separators=(',', ':'))))
    print(f'Built {args.output}: {len(positions)} positions, '
          f'{len(differing)} with differing choices.')


if __name__ == '__main__':
    main()
