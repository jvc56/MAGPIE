# Opponent-rack inference research, 2026-11-15 to 11-19

Research branch. Not for merging: it carries test-only hooks (oracle arms,
`full_rack_prior`, carry-over fields, replay tools) on top of
`feat/softmax-inference` (PR #938).

## Question

Does opponent-rack inference improve play, and which kernel should be the
default: hard margin 5 (main), softmax T8 (#938), a bag-dependent temperature
schedule, carry-over of the posterior between turns, or sim-steered weights.

## Results so far (CSW24, 4-ply sims, 100k rollouts per decision)

Decision runs measure 100 x the oracle win-probability difference per decision
at bag >= 8, with game-clustered 95% intervals. Always report win pp, utility
pp and equity together.

- `decide-fixed100k-sched`, 341 games: schedule - hard5 -0.019 +-0.043;
  T8 - hard5 -0.063 +-0.046; hard5 vs none +0.122 +-0.061.
  Recommendation given: keep hard5, do not merge #938 with T8 as default.
- `decide-fixed100k-carry` (seed 2128000000, arms h/g/m, pre-registered in
  `decide-fixed100k-carry/`): running until 07:00 PST Nov 20. At 91 games:
  carry - hard5 -0.004 +-0.083; carry - schedule -0.059 +-0.061;
  schedule - hard5 +0.056 +-0.066.
- Offline (ln weight on the true leave, relative to static softmax):
  carry-over +0.22 +-0.02 on moves with history, no time cost; 2-ply
  mini-sim on every rack +0.45 at ~500 rollouts per play, ~0 at 21.
- Tried and not useful: same-tiles quota, follow-up-play features,
  stratified rack sampling, a full-rack "exact" prior.

Better nats have not yet turned into better decisions.

## Code

- `src/ent/inference_args.h`, `src/impl/inference.c`: `carry_in`,
  `carry_in_kept`, `carry_in_floor`, `carry_out`. Softmax path only.
- `test/infer_decide_test.c`: harness `infdecide` (arms 0 none, h hard5,
  s T8, g schedule via `INFDEC_SCHEDULE`, m schedule + carry-over), replay
  tools `inftfit` (temperature grid; `INFTFIT_CARRY=1` for carry-over) and
  `infsfit` (mini-sim per rack). `decide_log_shape` prints `INFDEC_SHAPE`.
- The mini-sim tool is subject to the config's `-minp 500` floor unless
  `sample_minimum` is set.

## This directory

- `analysis/`: the scripts behind every table (`carry_primary.py` and
  `carry_cmp.py` take a run.log path; `sim*.py` read `infsfit` output;
  `wrong.py` tabulates how often each method cuts the true leave's weight).
  Some have absolute paths from the machine they ran on.
- `replay-inputs/`: logged control-arm games split in 12, stdin for the
  replay tools (`INFTFIT_SEED=2126000000`).
- `carry-engine-check/`: `inftfit` carry-over output, 5,434 moves with history.
- `run-scripts/`: how the offline collections were launched.

Raw run logs and the 640 MB of mini-sim output are not in git.
