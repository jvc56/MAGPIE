# Exact PAT inference with cached board features and census bounds

This opt-in evaluator accelerates PAT softmax experiments without changing the
model, margin, temperature, or posterior. It is available through `infbench`;
normal inference and autoplay still use their existing model and settings.

## Algorithm

`PATInference` owns a context for one board, observer rack, score, bag, lexicon,
PAT model/class mask, and observed placement. Only the candidate opponent rack
changes. Each worker must own a separate context.

1. Cache the board's premium-square/window geometry, weighted feature maps,
   board tile counts, opening penalties, and utility tables. Refresh unseen
   counts, stage scaling, rack-sensitive feature penalties, hook ordering, and
   lane bounds for each candidate rack. A separate movegen entry point copies
   this prepared context into exact search instead of scanning the board again.
2. Optionally build the score census once. For each playable tile multiset,
   combine its maximum raw score with the candidate leave value, the maximum
   conservative PAT defense bound over lanes, and the score/tiles-specific
   utility upper bound. Include legal exchanges and pass in the bound.
3. The observed placement is scored exactly. Remove it from its census cell,
   replacing that cell with the best alternative raw score from an exhaustive
   move generation using exactly those played tiles. This avoids letting a
   loose bound on the already-known move force a search.
4. If every alternative's upper bound is at most the observed equity, regret
   is zero. If a legal exchange or a revalidated previously found placement
   exceeds observed equity plus the margin, the rack has zero likelihood.
   Otherwise run exact PAT best-move generation.

A rejected result contains a lower bound, not an exact regret. It is valid for
that margin and smaller margins only. No arbitrary top-N placement list is used
as an upper bound. The four carried placements supply lower bounds only.

The census path currently requires a nonempty classic board, a nonempty bag,
shared lexica, and a supported alphabet. Other cases use exact move generation
with cached PAT setup. In particular, opening census scores include legacy
opening adjustments, so they are not used as raw-score PAT upper bounds.

## Reproduction

Build with `make magpie_test BUILD=no_pgo_release`.

```
./bin/magpie_test infbench:4:20:2026130000:pathybrid
./bin/magpie_test infbench:4:20:2026130000:patreference
./bin/magpie_test infbench:4:20:2026130000:pathybridfast
```

These modes sample placements leaving 1–4 tiles, using paired games against the
same reproducible 1,000-iteration two-ply policy as calibration. The hybrid uses
census for four-tile leaves and cached exact PAT for smaller leaves. `pathybrid`
checks each candidate against the original full PAT evaluation, cycling margins
10/20/40/80. `pathybridfast` evaluates at margin 80 and produces the entire
calibration grid from those results. `patreference` provides the original path
on the identical positions. Compare all `CALIBRATION` records, not timings.

`PATHYBRID` reports candidate racks, exact searches, zero-regret resolutions,
rejections, setup seconds and evaluation seconds. Setup must be included when
comparing performance. `CALTIMING` reports the reference or hybrid PAT evaluation
cost separately from static evaluation, game simulation and posterior reporting.

## Initial measurements (2026-11-03)

Four pairs (eight games), seed 2026130000, yielded 34 positions and 68,324
candidate racks. The verification run checked exact observed equity and either
exact best equity or the claimed bound at margins 10/20/40/80. For four-tile
leaves it resolved 5,797 zero-regret racks and rejected 4,711 out of 47,306,
avoiding 22.2% of exact searches across those margins.

Separate optimized runs used reference/hybrid/hybrid/reference order, with no
concurrent builds or benchmarks. All printed `CALIBRATION` records matched
exactly, including nats, KLV MSE, vowel-count MSE, tile-score MSE, tile-count MSE,
and exclusion/fallback indicators, across every grid setting. At the timing
run's margin 80, 5,797 four-tile racks (12.3%) avoided search, all by zero-regret
bounds; none were rejected by a lower bound.

Mean seconds per run, including hybrid context/census setup:

| Kept leave size | Reference PAT | Hybrid PAT | Reduction |
|---|---:|---:|---:|
| 1 | 0.005063 | 0.004232 | 16.4% |
| 2 | 0.104815 | 0.091432 | 12.8% |
| 3 | 0.814627 | 0.687693 | 15.6% |
| 4 | 2.144391 | 2.087186 | 2.7% |
| All | 3.068895 | 2.870543 | 6.5% |

These are evaluator timings, not a match-strength gain. The one-tile sample is
particularly small (123 racks). Four-tile census setup averaged 0.157 seconds;
the conservative bounds and their lookup cost leave only a small net saving at
margin 80. This does not establish census as faster than cached PAT alone, nor
justify enabling PAT by default. Raw runs and frozen binaries are under
`data/benchmarks/pat-hybrid-20261103/` (ignored local artifacts).


The ordinary-path control used `infbench:2:20:2026130000:calibrate` on the
pre-change binary and the new binary, in old/new/new/old order. All calibration
records matched. Mean static evaluator time was 0.719639 versus 0.724054 seconds
(+0.61%); ordinary PAT was 2.126369 versus 2.130760 seconds (+0.21%). These small
movements are within 1%, rather than evidence of an exact zero timing difference.
The shared movegen helpers are forced inline so the ordinary path has no extra
calls, cache fields, per-rack copies or feature-flag branches.

Paired CPU profiles of the initial implementation, with matching dSYMs and the
profiler's symbol checks, showed reduced PAT scans and utility setup, offset by
census/KLV lookups. The subsequent inline adjustment removed ordinary-path call
overhead; the table above was remeasured after that adjustment.

Validation includes rack-reload versus fresh-context comparisons across blanks,
repeated letters and returning to an earlier rack; exact-rack oracle checks;
full-grid posterior parity; and sanitizer coverage of the hybrid, including its
zero-regret and rejected outcomes. No match-strength claims are made here.

The 15×15 ASAN/UBSAN checks cover `pateval`, `patwmp`, `patutility`, `movegen`,
`infer`, `infbenchmetrics`, `census`, and hybrid oracle runs. The 21×21 sanitizer
super suite passes. The hybrid benchmark itself was not run on 21×21: the local
CSW21 WMP has 15×15 dimensions and the benchmark correctly rejects it.
