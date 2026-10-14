# 1PEG choosers under static, two-ply and exact endgames, 2026-10-14

PR #807 (`notes/peg1-player-20261014`) scored its 1PEG choosers in completed
games. After the chooser's move, the rest of the game was played by a common
continuation: static top-1 for both players, or a fixed two-ply endgame search
for both. A MAGPIE player solves a bag-empty endgame exactly, so neither
continuation is what the chooser would face in play. This study measures how
much the continuation changes a chooser's measured gain over static, and which
of the PR's numbers that affects.

The PR's networks and harness are not in git, so the choosers here are
stand-ins built from main: the zero-learning one-ply reply rule the PR
describes, the depth-2 endgame teacher its networks were trained toward, and
MAGPIE's own PEG solver.

## Summary
- **The static continuation is biased. The two-ply one is nearly exact.** Over
  1,863 (board, move) pairs, static top-1 play after the 1PEG move left the
  mover 8.0 points and 4.8% wins better off than optimal play,
  and got the win/loss/tie result wrong 5.6% of the time. Two-ply search
  was off by 1.5 points and got 0.9% wrong.
- **For reply-feature choosers the bias mostly cancels.** Each chooser is
  compared with static on the same continuation, so a bias they share drops
  out. The one-ply reply rule and the depth-2 teacher stand in for the PR
  networks' features and labels. Their gain over static is 0.1–1.1 points of
  win rate larger under the static continuation than under exact play, within
  noise, with the same points. Under exact play the one-ply rule gains
  +1.5% wins, +4.3 points, the depth-2 teacher +4.2% wins, +9.2 points (margin first) and +5.5% wins, +7.7 points (wins
  first).
- **The greedy-seed qualification is an artifact of the continuation.** Under
  the static continuation PEG's greedy seed gains +6.8% wins and +8.7 points and looks better
  than the depth-2 teacher. Under exact play it gains +0.3% wins and -2.1 points, and the
  teacher beats it by +4.0% wins [+2.3, +5.8] and +11.2 points. Its passes (21 boards) look 20
  points better than static's pick under the static continuation and are 21
  points worse under exact play.
- **MAGPIE's own PEG is the comparator that matters.** On 109 boards its
  default schedule beats the depth-2 teacher by +4.1% [+0.9, +8.3] wins under exact
  play (margin first; +2.8% [+0.0, +6.4] against wins first). It takes a median 201 s
  per decision on one thread.
- **For PR #807:** quote the two-ply columns. Here they sit within about 0.5%
  wins and 0.6 points of exact for reply-feature choosers. The static-endgame
  headline, the static full-game score and the greedy-seed qualification should
  be replaced. The networks themselves still need a rerun with an exact
  continuation (see below).

## Method
- **Boards.** NWL23 static self-play (top equity for both players). A board is
  the first turn with exactly one tile in the bag and both racks full. Board
  *k* is the *k*-th such game from seed 202610141700 + attempt, so every board
  is reproducible from its seed. 768 boards were planned; 577 finished
  before the cutoff (see Caveats).
- **Information rule.** Every chooser sees a copy of the position whose hidden
  tiles were replaced by a canonical assignment from the unseen pool: the
  lowest unseen letter in the bag, the rest on the opponent's rack. Nothing it
  reads can depend on the actual last tile or the opponent's rack. MAGPIE's
  PEG enumerates scenarios from the mover's unseen pool on its own.
- **Choosers.**
  - `static`: static top-1, the baseline.
  - `score`: highest score.
  - `one-ply reply rule`: static top 64, ranked by the spread after the move
    minus the expected highest-scoring opponent reply over every possible
    last tile (plus twice the mover's rack when the reply goes out).
  - `depth-2 teacher`: static top 64, ranked by a 2-ply `endgame_solve` per
    possible last tile, margin first or wins first. This is the label the
    PR's networks learn.
  - `PEG greedy seed`: MAGPIE `peg_solve` stage 0 over all moves or the
    static top 64. One thread, full enumeration, rational opponent, no
    nesting, as in the PR.
  - `MAGPIE PEG, default schedule`: `peg_solve` with autoplay's default
    schedule (halving stages 32/16/8/4/2, nested lookahead). One thread;
    boards 0–127 only, for cost.
  - `exact teacher (top 8)`: like the depth-2 teacher but with a full-depth
    solve, over the static top 8. A side run on boards 0–63.
- **Continuations.** Each distinct pick is played on the real position,
  drawing the real last tile, and the game is finished three ways:
  - `static`: static top-1 for both players to the end.
  - `two-ply`: a 2-ply `endgame_solve` for the player on turn each turn, with
    a fresh 2-ply table per pick.
  - `exact`: one full-depth `endgame_solve` from the post-move position. Its
    value is the final spread under optimal play by both sides. Every solve
    records whether it proved its value.

  A pass leaves the tile in the bag. The opponent's next turn is then static
  top-1 (`static`, `two-ply`) or MAGPIE's default PEG pick (`exact`), and the
  game continues as above once the bag is empty.
- **Metrics.** Per decision, the chooser's result minus static top-1's on the
  same board under the same continuation. Wins count W = 1, T = ½, L = 0;
  points are final mover spread. 95% CIs come from 10,000 bootstrap resamples
  of boards.

## Results
Boards: 577. Exact solves flagged unproven: 0.00% (runs before the flag count as proven).

### Change per 1PEG decision vs static top-1

| chooser | boards | differs | static: wins | static: points | twoply: wins | twoply: points | exact: wins | exact: points |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| score only | 577 | 0.56 | -0.87% [-2.7, +0.9] | -0.77 [-2.3, +0.8] | -0.78% [-2.3, +0.8] | -0.69 [-2.2, +0.8] | -1.21% [-2.8, +0.3] | -1.01 [-2.5, +0.5] |
| one-ply reply rule | 577 | 0.64 | +2.34% [+0.3, +4.4] | +4.35 [+2.4, +6.3] | +1.99% [+0.3, +3.7] | +4.85 [+3.2, +6.7] | +1.47% [-0.2, +3.2] | +4.25 [+2.6, +6.0] |
| depth-2 teacher, margin first | 577 | 0.64 | +5.37% [+3.1, +7.6] | +8.69 [+6.7, +10.8] | +4.42% [+2.6, +6.3] | +9.49 [+7.7, +11.3] | +4.25% [+2.4, +6.2] | +9.16 [+7.4, +11.0] |
| depth-2 teacher, win first | 577 | 0.65 | +5.55% [+3.3, +7.8] | +7.44 [+5.4, +9.5] | +5.72% [+3.8, +7.8] | +7.99 [+6.2, +9.8] | +5.46% [+3.6, +7.5] | +7.67 [+5.9, +9.4] |
| PEG greedy seed, static top 64 | 577 | 0.79 | +6.33% [+4.0, +8.8] | +8.14 [+5.8, +10.5] | +2.08% [+0.2, +4.1] | +1.59 [-0.4, +3.7] | +1.82% [-0.1, +3.8] | +1.06 [-0.9, +3.1] |
| PEG greedy seed, all moves | 577 | 0.83 | +6.85% [+4.5, +9.4] | +8.67 [+6.2, +11.2] | +1.99% [+0.1, +4.0] | +0.03 [-2.1, +2.3] | +0.26% [-1.6, +2.3] | -2.08 [-4.2, +0.1] |
| MAGPIE PEG, default schedule | 109 | 0.65 | +8.26% [+1.8, +14.7] | +9.60 [+5.0, +14.6] | +9.17% [+4.6, +14.7] | +10.48 [+6.2, +15.2] | +9.17% [+4.6, +14.7] | +10.42 [+6.3, +15.1] |

### How much each continuation moves the measured gain

| chooser | static minus exact: wins | points | two-ply minus exact: wins | points |
|---|---:|---:|---:|---:|
| score only | +0.35% [-1.0, +1.8] | +0.24 [-0.7, +1.2] | +0.43% [+0.0, +1.0] | +0.32 [+0.1, +0.6] |
| one-ply reply rule | +0.87% [-0.7, +2.5] | +0.10 [-0.8, +1.0] | +0.52% [+0.0, +1.2] | +0.59 [+0.3, +0.9] |
| depth-2 teacher, margin first | +1.13% [-0.4, +2.7] | -0.48 [-1.5, +0.6] | +0.17% [+0.0, +0.5] | +0.33 [+0.1, +0.6] |
| depth-2 teacher, win first | +0.09% [-1.5, +1.6] | -0.23 [-1.3, +0.8] | +0.26% [-0.2, +0.9] | +0.32 [+0.0, +0.6] |
| PEG greedy seed, static top 64 | +4.51% [+2.5, +6.6] | +7.07 [+5.6, +8.6] | +0.26% [+0.0, +0.7] | +0.53 [+0.2, +0.9] |
| PEG greedy seed, all moves | +6.59% [+4.2, +9.0] | +10.75 [+8.9, +12.7] | +1.73% [+0.8, +2.9] | +2.12 [+1.2, +3.2] |
| MAGPIE PEG, default schedule | -0.92% [-4.6, +1.8] | -0.83 [-2.6, +0.9] | +0.00% [+0.0, +0.0] | +0.06 [-0.2, +0.3] |

### Cheap continuations against the exact solve

1863 (board, move) pairs.

| continuation | mover spread minus exact | mean abs error | W/T/L agrees with exact | mover wins minus exact |
|---|---:|---:|---:|---:|
| static | +8.02 | 9.56 | 94.4% | +4.75% |
| twoply | +1.53 | 1.90 | 99.1% | +0.78% |

### Boards where MAGPIE's default PEG ran (109)

| chooser | boards | differs | static: wins | static: points | twoply: wins | twoply: points | exact: wins | exact: points |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| score only | 109 | 0.57 | +0.92% [-2.8, +4.6] | +0.70 [-3.2, +4.9] | +0.92% [-1.8, +3.7] | +1.15 [-2.4, +5.0] | +0.92% [-1.8, +3.7] | +0.53 [-3.2, +4.6] |
| one-ply reply rule | 109 | 0.66 | +1.83% [-2.8, +7.3] | +5.73 [+0.3, +11.4] | +1.38% [-2.3, +5.5] | +6.84 [+2.2, +11.8] | +1.38% [-2.3, +5.5] | +6.20 [+1.5, +11.4] |
| depth-2 teacher, margin first | 109 | 0.68 | +6.42% [+1.8, +11.9] | +9.49 [+4.7, +14.6] | +5.05% [+0.9, +9.6] | +10.98 [+6.4, +16.0] | +5.05% [+0.9, +9.6] | +10.51 [+5.9, +15.7] |
| depth-2 teacher, win first | 109 | 0.68 | +8.26% [+2.8, +14.7] | +9.12 [+4.4, +14.1] | +6.42% [+1.8, +11.9] | +9.14 [+4.9, +13.9] | +6.42% [+1.8, +11.9] | +8.71 [+4.4, +13.4] |
| PEG greedy seed, static top 64 | 109 | 0.80 | +9.17% [+2.8, +15.6] | +8.97 [+3.7, +14.8] | +1.83% [-2.8, +6.4] | +4.28 [-0.7, +9.9] | +1.83% [-2.8, +6.4] | +3.64 [-1.5, +9.2] |
| PEG greedy seed, all moves | 109 | 0.83 | +11.01% [+4.6, +18.3] | +10.83 [+5.1, +16.9] | +1.38% [-2.8, +6.0] | +2.91 [-2.4, +8.6] | +0.46% [-4.1, +5.5] | +1.32 [-3.9, +7.0] |
| MAGPIE PEG, default schedule | 109 | 0.65 | +8.26% [+2.8, +14.7] | +9.60 [+4.9, +14.7] | +9.17% [+3.7, +14.7] | +10.48 [+6.1, +15.2] | +9.17% [+4.6, +14.7] | +10.42 [+6.2, +15.3] |

Pass picks: greedy_all 21

## What this means for PR #807
- **Summary bullet 2** (+3.3% wins, +6.3 points per decision with a static
  endgame): replace with the two-ply figures (cascade-u +2.93%, +5.79). In the
  stand-ins, static overstates reply-feature gains by 0–1% wins. The PR's own
  static-minus-two-ply gap for cascade-u is +0.41% wins and +0.54 points.
- **Full games** (50.36% static, 50.31% two-ply): quote 50.31%. Only the 1PEG
  decision differs between seats, so the full-game score follows the
  per-decision result.
- **Greedy-seed qualification** (wins more often than any network, +6.0%,
  when the endgame is played greedily): remove. Under exact play the greedy
  seed is close to static (+0.3% wins over all moves, +1.8% over the top 64),
  and its passes are its worst picks. The PR's two-ply comparison (cascade-u
  +1.4–1.7% wins over the greedy seed) is probably an underestimate. Here the
  depth-2 teacher's margin over it grows from +2.4% (two-ply) to +4.0% (exact),
  partly because a two-ply continuation still plays the opponent's post-pass
  1PEG statically.
- **Utility mode:** in the depth-2 teacher, ranking by wins first beats margin
  first by only +0.2% wins under the static continuation. Under two-ply and
  exact play it gains +1.2–1.3% wins at −1.5 points. The utility table's
  two-ply columns are the relevant ones.
- **Headroom** (+4.1% for win-first teacher picks vs +3.0% margin first, static
  endgame): rerun under exact play. The stand-in teacher gives +5.5% vs +4.2%
  under exact play, so the conclusion survives.
- **Recommended default:** on the PR's two-ply numbers cascade-u (+2.93%) and
  strong-u (+2.95%) tie, and cascade-u is about 6× cheaper. The recommendation
  still holds.
- **Intro:** quote the previous network's two-ply result (50.4%, −0.9).
- **Unaffected:** the cost table, the regret table against depth-2 labels, the
  teacher-quality section and the information-rule checks.
- **Missing comparator:** MAGPIE's default PEG solve is what plays a 1PEG in
  production, and under exact play it is the strongest chooser here. It belongs
  in the comparison, at its cost.

## Fixing the PR's evaluation
1. **Score with one exact solve.** After the chooser's move is played with the
   real last tile, call `endgame_solve` once from the post-move position with
   `plies` 25. The solver stops by itself once the root value is proven, so 12
   and 25 gave identical values and times. When the opponent is on turn, the
   mover's final spread is `spread_now - value`. Record whether the value was
   proven: `endgame_ctx_get_progress` depth below `plies`. Reference:
   `p1c_exact` in `test/peg1_continuation_test.c`.
2. **Handle passes.** A pass leaves the tile in the bag, and `endgame_solve`
   refuses a non-empty bag. The opponent's 1PEG reply needs a rule; here it is
   MAGPIE PEG's pick, then the exact solve.
3. **Budget for the tail.** Exact solves took a median 0.6 s, mean
   44 s, p90 64 s, p99 1,142 s and max 2,388 s on one
   thread. With about 3.2 distinct picks per board that is roughly 140
   core-seconds per board: about 320 core-hours for 8,192 boards, or about 40
   for a 1,024-board confirmation run. Dedupe picks per board and give the
   slow solves several threads.
4. **Full games.** For games that reach a 1PEG, finish with the exact solve
   (or autoplay's `-eplies1`/`-eplies2`). The pair score follows the
   per-decision result.
5. **Two-ply as a proxy.** It tracks exact play for reply-feature choosers.
   It does not for choosers that pass or lean on greedy play: the greedy seed
   over all moves is still +1.7% wins and +2.1 points optimistic under two-ply.

## Caveats
- These are stand-ins, not the PR's networks. The networks' features follow
  the one-ply rule and their labels are the depth-2 teacher's. Their sensitivity
  to the continuation should be similar, but that is inferred, not measured.
- The two-ply continuation here is MAGPIE's `endgame_solve` at 2 plies per
  turn. The PR harness's "fixed two-ply endgame search" may differ in detail.
- 577 of 768 planned boards finished before the cutoff. The missing ones are
  each shard's middle boards (shards ran from both ends) plus the boards in
  progress at the cutoff, which lean toward long endgames.
- The first ~105 boards were scored before two harness fixes. They shared one
  2-ply table across picks, which affects only two-ply numbers. Boards where a
  pick passed crashed before the fix and were rescored after it. Those first
  boards also predate the exact-solve proof flag; every flagged solve since
  was proven.
- MAGPIE's default PEG ran only on boards 0–127, single-threaded, for cost.
  The exact teacher over the static top 8 finished only 8 boards in the side
  run, too few to say whether exact labels would change the teacher's picks,
  so it is left out of the tables.
- Boards are NWL23 static self-play like the PR's, from different seeds.

## Reproduction
- Code: branch `claude/peg1-continuation-check`. The harness is
  `test/peg1_continuation_test.c` (on-demand test `peg1cont`), and the
  scripts are in `tools/peg1cont/`: `run_vm.sh`, `analyze.py`, `tables.py`.
- One shard of a run, from the repo root after `make magpie_test
  BUILD=no_pgo_release`:

  ```
  PEG1C_OUT=out.jsonl PEG1C_SHARD=0 PEG1C_NSHARDS=32 PEG1C_BOARDS=768 \
    PEG1C_PEGFULL=128 ./bin/magpie_test peg1cont
  ```
- Raw results are on the branches `claude/peg1cont-results-vm{0..7}`,
  `-vm{0..7}-r1` and `-vm{0..7}-rev`, one JSONL record per board. Boards
  scored more than once merge by seed.
- `python3 tools/peg1cont/tables.py 'results/*/results/*.jsonl'` rebuilds
  every table here.
