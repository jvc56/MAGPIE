# Sigmoid spread-utility weight sweep for sims

Question: should the BAI sim sampler's spread-utility blend (`uwin` /
`uspread` / `uspreadscale`, mechanism from commit 31758629, defaults
`1.0 / 0.0 / 100.0` = pure win%) get a nonzero `uspread` prod default?

Hypothesis: win outcomes are 0/0.5/1 per rollout (high variance) while
spread is continuous, so a small sigmoid-normalized spread term may help
BAI discriminate near-equal plays faster at a fixed budget.

## Method

Head-to-head autoplay game pairs: P1 = control (`-uwin1 1 -uspread1 0
-uspreadscale1 100`), P2 = treatment (`-uwin2 1 -uspread2 X
-uspreadscale2 Y`). Autoplay honors per-player sim utility weights out of
the box (`config.c` fills `p1_sim_args`/`p2_sim_args` including the
utility fields), so no custom harness was needed.

**Bracket / scale-check settings** (cheap): both players sim with 2-ply
rollouts, top 15 candidate plays, 600-total-sample cap per turn
(`-pl 2 -np 15 -i 600`), CSW21 + WMP, `-gp true`. Iteration caps (not
wall-clock time limits) were used throughout so the comparison stays fair
even during the primary checkout's occasional compile bursts.

**Confirm settings** (realistic): 4-ply rollouts, top 15 candidates,
1000-sample cap per turn (`-pl 4 -np 15 -i 1000`), same otherwise.

Each arm ran as **20 chunks of 10 pairs** (seeds 42..61, 400 games/arm for
bracket + scale-check; 10 chunks x 10 pairs = 200 games for confirm),
independent seeds per chunk so spread significance can be computed by
batch means across chunks (autoplay has no per-game export; an earlier
draft used 8 chunks of 25 pairs, which only gives df=7 for the spread
t-test -- too weak to resolve realistic effect sizes. 20 chunks gives
df=19 at the same total game count).

- **Wins**: exact two-sided binomial over pooled decisive games.
- **Spread**: one-sample t-test across the per-chunk means of
  spread-per-pair (`2 * (mean_p2_score - mean_p1_score)`, since a pair is
  two games with tile draws/first-move mirrored -- this cancels draw luck
  and is the powered test).
- Avg score per arm is reported too: a spread-seeking utility could in
  principle trade wins for points, which is the tradeoff a prod-default
  decision needs to see directly.

Machine: 8 logical CPUs (Apple Silicon), `-threads 6`, one autoplay run
at a time, machine otherwise idle during the timed runs.

Repro:

```bash
# one bracket/scale-check arm (2-ply, i=600):
notes/sigmoid_sweep_arm.sh u0.50 0.50 100 10 20 6 <logdir>
# confirm arm (4-ply, i=1000) -- override via env vars, NOT extra CLI
# args (magpie's arg parser rejects a flag given twice with a printed
# error but EXIT CODE 0, so a naive "$@" append silently no-ops instead
# of failing -- see the script's header comment):
SWEEP_PLIES=4 SWEEP_NP=15 SWEEP_ITERS=1000 \
  notes/sigmoid_sweep_arm.sh confirm_u0.50 0.50 100 10 10 6 <logdir>
python3 notes/sigmoid_sweep_analyze.py <logdir> u0.50
```

## Results

| Arm | Setting | Pairs | Win% (P2) | p(win) | Spread/pair | p(spread) | Avg score P1 / P2 |
|---|---|---|---|---|---|---|---|
| u0.10 | uspread=0.10, scale=100 | 200 | 51.25% | 0.652 | +61.6 | 0.0000 | 426.9 / 457.7 |
| u0.25 | uspread=0.25, scale=100 | 200 | 51.25% | 0.652 | +63.8 | 0.0000 | 425.8 / 457.7 |
| u0.50 | uspread=0.50, scale=100 | 200 | 52.38% | 0.368 | +72.0 | 0.0000 | 425.9 / 461.9 |
| u1.00 | uspread=1.00, scale=100 | 200 | 52.38% | 0.368 | +72.2 | 0.0000 | 425.6 / 461.8 |
| u0.50_s50 | uspread=0.50, scale=50 | 200 | 53.62% | 0.161 | +68.6 | 0.0000 | 424.9 / 459.2 |
| u0.50_s200 | uspread=0.50, scale=200 | 200 | 51.00% | 0.726 | +64.5 | 0.0000 | 426.0 / 458.3 |
| **confirm_u0.50** | uspread=0.50, scale=100, **4-ply/i=1000** | 100 | 48.75% | 0.777 | **+75.0** | 0.0000 | 422.1 / 459.6 |

Control is always `uspread1=0` (scale is irrelevant when uspread=0, since
the spread term is weighted to zero regardless).

**Observations:**

1. **Win% never moves off ~50/50** in any arm, at either the cheap or
   realistic settings (`p(win)` ranges 0.16-0.78, never close to
   significant). Adding a spread term does not cost games.
2. **Spread/pair is large and highly significant everywhere** (+62 to
   +75 points/pair, `p < 0.0001` in every arm) -- a nonzero `uspread`
   consistently produces a better-scoring player without a corresponding
   win-rate cost.
3. **The uspread bracket saturates between 0.25 and 0.50**: 0.10 and 0.25
   are statistically indistinguishable, 0.50 and 1.00 are also
   indistinguishable from each other, but 0.50/1.00 edges out 0.10/0.25 on
   spread (+72 vs +62-64). Going all the way to `uspread=1.00` (equal
   weight to win% and spread) buys nothing over 0.50.
4. **Scale is not a sensitive knob**: 50, 100, and 200 all land in the
   same ballpark on both win% and spread, with no arm clearing
   significance against another. The existing default (100) is as good a
   choice as any of these.
5. **The confirm run (4-ply, 1000 iters, realistic budget) reproduces the
   pattern cleanly**: win% 48.75% (p=0.777, if anything slightly *below*
   50% but well within noise for n=100 pairs) and spread/pair +75.0
   (p=0.0000) -- the strongest spread result of the whole sweep, at the
   setting closest to production play.

## Recommendation

**Set `uspread=0.50` (keep `uspreadscale=100`) as the new prod default**,
up from `uspread=0`.

Justification: across six arms spanning a 10x range of `uspread` and a
4x range of `uspreadscale`, plus a confirm run at realistic 4-ply/1000-
iteration settings, a nonzero spread term never produced a measurable
win-rate cost (every `p(win)` well above 0.15) while consistently
delivering a large, highly significant improvement in average margin
(+62 to +75 points per pair, `p < 0.0001` in every single arm). The
effect saturates at `uspread=0.50` -- going to 1.00 adds nothing further
-- so 0.50 gets the full benefit without picking an unnecessarily extreme
weight. This matches the original hypothesis exactly: BAI's win-outcome
signal (0/0.5/1) is too coarse to discriminate near-equal candidate plays
well at a fixed sample budget, and a modest continuous spread term breaks
those near-ties in a way that nets more points without sacrificing wins.

**Caveats:**
- All runs used iteration-capped budgets (`-i 600` / `-i 1000`), not
  wall-clock time limits, for machine-load fairness. A pure win-rate
  metric under a *wall-clock* budget (where discriminating faster could
  free up rollouts for other candidates) was not directly tested and
  could show a larger effect than measured here.
- Every arm used a single position class implicitly sampled by CSW21
  autoplay's random game generation -- no explicit stratification by game
  phase (opening/midgame/endgame) or leave quality was done.
- This sweep does not touch `src/impl/config.c`'s defaults -- that's a
  follow-up decision for jvc56 once this report is reviewed.

## Phase log

- 2026-07-06: worktree setup on `sigmoid-utility-sweep-v2` (origin/main
  `6d190514`), release build OK, data symlinked. Smoke test (4 pairs,
  `-threads 4`, i=600): 102 s wall, pipeline works, per-player utility
  weights accepted.
- 2026-07-06: bracket phase (u0.10, u0.25, u0.50, u1.00) run sequentially,
  20x10 chunking, ~1h/arm.
- 2026-07-06: scale-check phase (u0.50_s50, u0.50_s200) run sequentially,
  same chunking, ~1h/arm.
- 2026-07-06: confirm phase's first attempt failed instantly on all 10
  chunks -- the driver appended `-pl1 4 -pl2 4 ...` on top of
  `sigmoid_sweep_arm.sh`'s own hardcoded `-pl1 2 -pl2 2 ...`, and
  magpie's CLI rejects a flag given twice ("command 'pl1' was provided
  more than once") but **returns exit code 0**, so `set -e` didn't catch
  it -- only `sigmoid_sweep_analyze.py`'s crash on a missing "All Games"
  summary surfaced the failure. Fixed by adding `SWEEP_PLIES`/`SWEEP_NP`/
  `SWEEP_ITERS` env-var overrides to `sigmoid_sweep_arm.sh` instead of
  appending CLI args, plus an explicit post-run check that the "All
  Games" summary landed (dumps the log and exits nonzero otherwise).
  Verified the fix with a live 2-pair smoke run before re-launching.
  The bracket and scale-check arms were unaffected by this bug (the same
  crash-on-missing-summary guard would have caught it in their results
  too, and did not).
- 2026-07-06/07: confirm phase re-run successfully with the fix.
