# Candidate diversity research checkpoint — September 30, 2026

This branch preserves the research implementation, completed measurements, and
candidate explorers for later focused PRs. It is a research checkpoint, not a
claim that these experimental controls are ready to ship. The engine worktree is
`magpie-pat-simcmp`, branch `claude/pat-sim-vs-threat`; its starting commit was
`98a5ada2`. The pass-relative source snapshot had the same tracked sources as
that worktree except for `test/pat_threat_games_test.c`. The checked-in harness
now also includes the latest targeted blocking study's same-count control and
configurable saved-position minimum bag.

## What was measured

- 15-second selection simulations with no-PAT static rollouts, normally starting
  from 25 static candidates. Candidate counts can grow adaptively through unions.
- Blocking is measured against passing on the same sampled opponent racks:
  `1.4 * mean(best opponent placement after pass - after candidate)` for the wide
  admission score. It is a score difference in points, not an absolute reply
  penalty. The original absolute-reply experiments remain in the initial archive
  as superseded exploratory evidence.
- Setup uses paired candidate leave/refill draws in the candidate and pass
  branches, including the opponent reply, before comparing our best follow-up
  placement: wide admission uses `0.75 * mean(follow-up after candidate - after
  pass)`. See the frozen protocol and harness for the draw-conditioning details.
- Eligible exchanges receive a quota of five within a generous 35-point static
  margin. Protocols specify each experiment's exact reference pool and controls.
- Zero-change positions are included. Identical candidate pools reuse a completed
  selection simulation; provenance is recorded in the reuse JSON files. For the
  overnight study use `matched_effective.csv`, retaining raw `m0..m7.csv` for audit.

## Main results

| Prespecified comparison | Independent positions | Mean reference WP gain (percentage points) | 95% bootstrap interval | Two-sided paired t p |
|---|---:|---:|---:|---:|
| Setup vs exactly equal-count static | 1,200 | +0.029236 | [+0.010523, +0.051509] | 0.005800 |
| Blocking vs exactly equal-count static, bag 5–29 and lead 40–80 | 400 | +0.033310 | [+0.000705, +0.074081] | 0.078382 |

The setup result rejects the prespecified null at 5%, with much of the effect
concentrated late in the game. The targeted blocking comparison does not reject
at 5%. Its bootstrap interval barely excludes zero, but the prespecified paired
t test does not; do not select a favorable significance label between methods.
Only 15 of the 400 blocking choices differ, so the result is sparse. The bag-five
subset contains 12 positions, and bag-six another 11; those subgroups are too
small for a strong conclusion.

References used independent 60-second four-ply simulations. Deeper validation
used 47 outcome-blind novel-choice cases in the setup study and 20 in the blocking
study; those enriched subsets are supplementary, not population strength tests.
The blocking subset's mean advantage was +0.438892 percentage points, with 95%
interval [-0.336232, +1.324412] and p=0.325732. This is a position-level simulation
study, not measured played-game strength.

Position 281 illustrates useful diversity: blocking admitted ACTA at static
rank 38. Its 16-ply reference WP was about 99.779%, roughly 4.142 percentage points
above the best play in the size-matched static pool. A separate 10,000-trial trace
found 9,974 wins, 20 losses, six ties and no unfinished games. ACTA leaves a rare
column-F bingo route through its A and a QUAIR→QUAIRS hook. These are static-policy
losses, not proof of forced losses against optimal endgame defence.

## Evidence and explorer pages

`results/` contains the final reports, summaries, paired outcomes and validations
for convenient review. `archives/` preserves the underlying text evidence:

- `initial-15s.tar.gz`: the first extended-budget exploratory work.
- `pass-relative.tar.gz`: the completed paired pass-relative rerun and controls.
- `overnight.tar.gz`: the frozen 1,200 independent-game setup confirmation,
  including recovery records, raw and effective matched results, deep references,
  and the full candidate explorer.
- `blocking.tar.gz`: the frozen 400-position late/ahead blocking confirmation,
  including 20 deep references and its final explorer.
- `acta-trace.tar.gz`: the ACTA diagnostic source snapshot and losing continuations.
- `prior-short-budget-union.tar.gz`: the preceding short-budget union results.
- `legacy-supplement.tar.gz`: launch scripts and explicitly superseded or partial
  CSV backups, kept separately from final analysis inputs.

Run `python3 verify_archives.py` to check every archived member against
`ARCHIVE_MANIFEST.json`. Original member bytes and their hashes are preserved.
Executable binaries, object files, dictionaries, tile models, Python bytecode,
OS metadata and coordinator locks are excluded. Historical build/input/result manifests still
record their original hashes and local paths; those manifests are evidence, not
a promise that a freshly compiled timed run reproduces identical output bytes.

To inspect the blocking explorer without running any experiment:

```sh
mkdir -p extracted/blocking
tar -xzf archives/blocking.tar.gz -C extracted/blocking
python3 -m http.server 8767 --directory extracted/blocking
```

Open `http://127.0.0.1:8767/candidate-pools-debug.html#primary/281` or `REPORT.html`.
For setup, extract `overnight.tar.gz` into `extracted/overnight` and serve that
directory on another free port. Pages preserve sortable full candidate lists,
provenance, static evaluation, blocking/setup adjustments and simulation results,
classic premium colors, tile scores, game scores, racks and unseen tiles. Choice
badges explicitly identify the four-ply reference choice even in the deep view.

## Reproduction and eventual PR split

The `scripts/` directory preserves the actual drivers, analysis, page builders
and diagnostic builder. Drivers contain original machine paths, frozen seeds,
deadlines and process/lock handling; some execute work at import time. Inspect
and adapt them deliberately before running. Do not rerun a completed study in
place or overwrite its evidence. Frozen per-study harness snapshots are inside
the archives; the formatted current harness is `test/pat_threat_games_test.c`.
The earlier code snapshot was `/Users/olaugh/sources/magpie-pat-pass-relative`.

Build the current harness with `make magpie_test BUILD=no_pgo_release`. The
on-demand entry is `./bin/magpie_test patcanddiversity`. Supply an explicit fresh
`PCD_IN` and `PCD_OUT`, and inspect the protocol/driver for the other `PCD_*`
settings. Run from the source checkout with no stale `settings.txt`. CSW24 data
and models remain external prerequisites; they are not embedded in this commit.

Suggested later PR boundaries are the sim candidate-source callback, experimental
move-generation controls, pass-relative candidate admission, and research/debug
tooling. The pre-existing staged four-ply/30-sample PlayChooser default changes
are also preserved and should be reviewed separately. Before production adoption,
split the large harness, make configuration and paths portable, run the complete
CI matrix and measure played-game effects and disabled-feature cost. Do not
silently port earlier phase, opening-exchange or absolute-reply knobs as defaults.

See `VALIDATION.json` for checks performed on this preservation commit. Existing
completed studies were audited and visually checked before this commit; only an
isolated short plumbing smoke test was added during preservation.
