# BAI Custom Utility — Design Plan

## Goal

Let BAI rank arms by a configurable utility, default `1.0·win% + 0.0·spread`
(backward compatible). Tunable to e.g. `0.7·win% + 0.3·spread_normalized` so
the simmer can prefer high-spread plays in lopsided positions and win%-only
in close ones.

## Where to inject

Single change point: `rv_sim_sample()` in `src/impl/random_variable.c`. After
it computes `wpct`, instead of returning `wpct` directly, return:

```
utility = w_winpct · wpct + w_spread · spread_norm(spread)
```

- `spread` is from `initial_player`'s perspective, already correctly signed
  across `plies % 2`.
- `spread_norm(s)`: maps `Equity` to roughly `[-1, +1]`. Default: clamp and
  divide, `clamp(s / spread_scale, -1, 1)`, `spread_scale = 100` (so ±100
  points saturates). Shift+scale into `[0, 1]` before blending so the sum
  stays bounded.

## Knobs

| Flag | Default | Meaning |
|---|---|---|
| `-uwin` | `1.0` | weight on win% |
| `-uspread` | `0.0` | weight on spread (after normalization) |
| `-uspreadscale` | `100.0` | spread points that map to ±1.0 |

With defaults, return value equals the existing `wpct`, so existing tests
and benchmarks are unaffected.

## Threshold caveat

BAI's GK16/AISTAR thresholds assume sub-Gaussian rewards in roughly `[0,1]`.
With non-zero `w_spread`, support widens unless we normalize. Approach:
shift+scale spread into `[0,1]` and renormalize the blend by
`w_winpct + w_spread`. User can supply unnormalized weights (e.g. `7, 3`)
and get the same answer as `0.7, 0.3`.

## Plumbing

- New fields on `SimArgs`: `double utility_w_winpct, utility_w_spread,
  utility_spread_scale`.
- `sim_args_fill(...)` defaults them to `(1.0, 0.0, 100.0)` — existing
  callers unchanged.
- Config sets them post-fill from `-uwin / -uspread / -uspreadscale`.
- `Simmer` copies them on create/reset.
- `rv_sim_sample` blends before returning.

## Tests

1. **Backward compat**: existing `sim_test`, `bai_test` pass unchanged with
   defaults.
2. **New unit test** `bai_utility_test`: same position, run sims with
   `(1, 0)` and `(0, 1)`, verify the chosen arm differs in a constructed
   position where win%-best and spread-best diverge (e.g., safe-and-low
   vs big-and-risky).
3. **Smoke bench**: small autoplay comparing `(1, 0)`, `(0.7, 0.3)`,
   `(0.5, 0.5)`. Watch for Elo / spread shifts.

## Phasing

1. **Plumbing only** — add fields, flags, default-zero spread weight,
   `rv_sim_sample` still returns plain wpct. CI green, no behavior change.
2. **Activate** — add normalization + blend in `rv_sim_sample`. Defaults
   still produce identical behavior; non-default weights actually change
   picks.
3. **Test + bench** — add `bai_utility_test`, run a smoke autoplay with
   `(0.7, 0.3)`.

## Why this is a prereq for simmed inference

The inner short sims in simmed inference need a per-rack "what would this
rack play?" answer. Pure win% from a 30-iter mini-sim is noisy; a weighted
blend that includes spread is more stable at low iteration counts (spread
is a less-quantized signal). The simmed-inference path can set
`-uwin 0.5 -uspread 0.5` (or whatever benches well) on its inner sims
while the outer sim keeps the user's chosen weights.
