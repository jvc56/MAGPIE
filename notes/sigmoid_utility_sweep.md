# Sigmoid spread-utility weight sweep for sims

Question: should the BAI sim sampler's spread-utility blend (`uwin` /
`uspread` / `uspreadscale`, mechanism from commit 31758629, defaults
`1.0 / 0.0 / 100.0` = pure win%) get a nonzero `uspread` prod default?

Hypothesis: win outcomes are 0/0.5/1 per rollout (high variance) while
spread is continuous, so a small sigmoid-normalized spread term may help
BAI discriminate near-equal plays faster at a fixed budget.

## Method

Head-to-head autoplay game pairs: P1 = control (`-uwin1 1 -uspread1 0`),
P2 = treatment (`-uwin2 1 -uspread2 X -uspreadscale2 Y`). Autoplay honors
per-player sim utility weights out of the box (config.c fills
`p1_sim_args`/`p2_sim_args` including the utility fields), so no custom
harness was needed.

Bracket settings (cheap): both players sim with 2-ply rollouts, top 15
candidate plays, 600-total-sample cap per turn (`-pl 2 -np 15 -i 600`),
default 99% stop condition, CSW21 + WMP, `-gp true`.

Each arm runs as 8 chunks of 25 pairs (seeds 42..49, 400 games/arm total)
so spread significance can be computed by batch means across chunks
(autoplay has no per-game export; chunk seeds are independent, pairing is
preserved within each chunk aggregate).

- Wins: exact two-sided binomial over decisive games.
- Spread: one-sample t-test across the 8 chunk means of spread-per-pair.

Machine: 8 logical CPUs (Apple Silicon, otherwise idle), `-threads 6`,
one autoplay run at a time. Per-game parallelism (default `mtmode`), so
each sim is single-threaded.

Repro:

```bash
notes/sigmoid_sweep_arm.sh u0.25 0.25 100 25 8 6 <logdir>
python3 notes/sigmoid_sweep_analyze.py <logdir> u0.25 ...
```

## Phase log

- 2026-07-06: worktree setup on `sigmoid-utility-sweep-v2` (origin/main
  6d190514), release build OK, data symlinked. Smoke test (4 pairs,
  `-threads 4`, i=600): 102 s wall, pipeline works, per-player utility
  weights accepted. ~33 s CPU per game at bracket settings.

## Results

(pending)

## Recommendation

(pending)

- 2026-07-06 16:36:13 u0.10: chunks=20 games=400 pairs=200 | P2 wins=204 P1 wins=194 ties=2 P2 win%=51.25 p_win=0.6520 | spread/pair=+61.60 p_spread=0.0000 | avg score P1=426.9 P2=457.7

- 2026-07-06 17:35:50 u0.25: chunks=20 games=400 pairs=200 | P2 wins=204 P1 wins=194 ties=2 P2 win%=51.25 p_win=0.6520 | spread/pair=+63.84 p_spread=0.0000 | avg score P1=425.8 P2=457.7

- 2026-07-06 18:34:41 u0.50: chunks=20 games=400 pairs=200 | P2 wins=209 P1 wins=190 ties=1 P2 win%=52.38 p_win=0.3675 | spread/pair=+71.97 p_spread=0.0000 | avg score P1=425.9 P2=461.9

- 2026-07-06 19:32:35 u1.00: chunks=20 games=400 pairs=200 | P2 wins=209 P1 wins=190 ties=1 P2 win%=52.38 p_win=0.3675 | spread/pair=+72.24 p_spread=0.0000 | avg score P1=425.6 P2=461.8

- 2026-07-06 21:09:26 u0.50_s50: chunks=20 games=400 pairs=200 | P2 wins=214 P1 wins=185 ties=1 P2 win%=53.62 p_win=0.1609 | spread/pair=+68.64 p_spread=0.0000 | avg score P1=424.9 P2=459.2
