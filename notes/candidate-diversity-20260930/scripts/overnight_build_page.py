#!/usr/bin/env python3
"""Publish a separate fresh-position explorer from audited complete records."""
import csv
import json
import os
from pathlib import Path

from overnight_prepare import D, REPO
os.environ['CANDIDATE_DEBUG_SOURCE'] = str(D)
os.environ['CANDIDATE_DATA_ROOT'] = str(REPO / 'data')
from build_candidate_debug import dataset, read_text_retry


def main():
    assert (D / 'summary.json').exists()
    summary = json.loads(read_text_retry(D / 'summary.json'))
    ids = {row['pos'] for row in summary['primary']['per_position']}
    arms = ['static25', 'threat_wide', 'setup_wide', 'signals', 'exchange', 'combined',
            'static_setup_count', 'static_combined_count']
    experiment = dataset(D, 'primary', [f'w{worker}.csv' for worker in range(8)] + ['matched_effective.csv'],
                         ['positions.cgp'], arms, allowed_ids=ids)
    experiment['positions'].sort(key=lambda item: item['id'])
    data_root = REPO / 'data'
    data = {'datasets': [experiment], 'premiums': [list(line) for line in (data_root / 'layouts/standard15.txt').read_text().splitlines()[1:]],
            'tileScores': {row[0]: int(row[3]) for row in csv.reader((data_root / 'letterdistributions/english.csv').open())}}
    template = read_text_retry(Path(__file__).parent / 'candidate_debug_template.html')
    template = template.replace('1217, KiSTFUL, BUSTI…', '870, NOIL, TEPAS…')
    template = template.replace('<option value="primary">Legacy setup + blocking · 320 positions</option><option value="quota">Legacy quotas · 40 fresh positions</option>',
                                f'<option value="primary">Fresh independent-game confirmation · {len(ids)} positions</option>')
    template = template.replace("tile_exchange:'Tile + exchange'", "tile_exchange:'Tile + exchange',static_setup_count:'Static · setup count',static_combined_count:'Static · combined count'")
    template = template.replace('The pass-relative rerun is in progress; this is the legacy data.', 'Fresh independent-game confirmation. Full 15-second selection sims, no-PAT static rollouts, and exactly matched static candidate counts.')
    template = template.replace('Blocking ranks plays by static equity minus 1.4 times the mean best opponent placement reply score. Setup ranks plays by static equity plus 0.75 times the setup component.',
                                'Blocking adds 1.4 times the mean reduction in the opponent’s best placement score versus passing. Setup adds 0.75 times the change in our best next-turn placement score after the opponent reply versus the matched pass branch. Both branches use the same sampled opponent rack and candidate leave/refill. Refills exclude the opponent rack. There are 64 sampled racks; a terminal opponent reply gives zero next-turn opportunity. The static count controls contain the same number of candidates as the corresponding setup or combined pool.')
    template = template.replace('setup = 3 × (exported setup value − static equity). A smaller blocking penalty ranks better; a larger setup bonus ranks better.',
                                'setup = 3 × (exported setup value − static equity). Positive blocking reduces opponent scoring versus pass; positive setup improves our matched next-turn scoring versus pass.')
    template = template.replace('Primary pool sims have a 15-second maximum and may stop early;', 'Primary pool sims run for a full 15-second budget;')
    template = template.replace('Repaired references replace seven original runs.', 'All references are fresh; deeper references use independently sampled 16-ply rollouts on the prespecified novel-choice subset.')
    template = template.replace("[1217,493,580,1100,2241,1755]", "dataset.positions.filter(item=>item.candidates.some(candidate=>!candidate.results[base()]&&candidate.results.setup_wide?.[4])).slice(0,6).map(item=>item.id)")
    template = template.replace("dataset.name==='primary'?1217:106", "dataset.positions[0].id")
    template = template.replace('Setup / blocking study', 'Fresh independent-game study')
    template = template.replace('In the quota study these are diagnostic values. ', '')
    template = template.replace('The fresh quota experiment adds a best placement for each tile count within 35 points, plus the exchange quota. ', '')
    template = template.replace('Other groups contain plays outside that anchor, labeled by every check that admitted them.',
                                'Other groups contain plays outside that anchor, labeled by the checks that admitted them and membership in the equal-size static controls. Static count tags identify the control pools.')
    output = D / 'candidate-pools-debug.html'
    output.write_text(template.replace('/*DATA*/null', json.dumps(data, separators=(',', ':'))))
    print(f'Built {output}: {len(ids)} audited positions, {output.stat().st_size:,} bytes.')


if __name__ == '__main__':
    main()
