# KLV3 contextual leave values

KLV3 extends an ordinary KLV2 with a small, additive contextual model. The
KLV2 value remains the rack-only baseline. KLV3 adjusts that value using the
public unseen tile multiset and the number of tiles the move will draw:

```
V(L, U, N, d) = KLV2(L)
              + sum_h L[h] * (
                    bias[pool_bin(N), d, h]
                  + sum_u weight[d, h, u] * U[u] / N)
```

`L[h]` and `U[u]` are tile counts, so repeated tiles contribute with their
actual multiplicity. There is one weight matrix for each draw count. The
public unseen multiset is the bag plus the opponent's rack; the model does not
use private knowledge of which unseen tiles are in either place. Moves that
draw no tiles deliberately receive no contextual adjustment.

This factorization is small and fast. It represents general pairwise
interactions between every held and unseen tile type without indexing every
possible unseen multiset. MAGPIE computes the adjustment for every held tile
once per position, after which each leave lookup adds one value per retained
tile.

## File format

A `.klv3` starts with an exact KLV2 body and appends:

1. Eight-byte magic `MAGKLV3\0`.
2. Four little-endian `uint32_t` fields: version, alphabet size, number of
   draw-count heads, and number of pool-size bins.
3. One little-endian `uint16_t` upper bound per pool-size bin.
4. Little-endian `float32` biases in
   `[pool_bin][draw_count][held_tile]` order.
5. Little-endian `float32` weights in
   `[draw_count][held_tile][unseen_tile]` order.

Both versions have eight draw-count heads, indexed 0 through 7. Draws of seven
or more use head 7. Version 2 additionally appends:

6. A cap quantile in parts per million and a minimum per-leave sample count,
   as two little-endian `uint32_t` fields.
7. One little-endian `float32` contextual-adjustment cap per ordinary KLV
   leave index.

The current trainer uses an empirical p99 model-residual cap for leaves with
at least 32 observations. Sparse observed leaves fall back to the p99 for
their draw-count head; unseen leaves use the model-wide p99. These are lossy
quantile bounds, not correctness guarantees. Final move equity is always
evaluated with the exact current-position KLV3 adjustment.

A requested leave name resolves to `.klv3` first and falls back to `.klv2`,
so existing configurations continue to work.

KLV3 still cannot reuse a rack-keyed cache of evaluated leave values across
positions: the same rack has a different value under a different public
unseen pool. WMP word existence data and every structural part of RIT remain
valid. A KLV3-capable RIT therefore stores:

- the ordinary KLV2 base values in leave-map order;
- model fingerprints, so stale or mismatched tables fail closed;
- per-rack maxima of `KLV2(leave) + cap(leave)` over all canonical subracks.

The capped maxima occupy the existing `best_leaves` and
`nonplaythrough_best_leave_values` fields in a model-specific RIT. Keeping
them inline matters: an earlier v13 prototype appended two overlay arrays
totaling about 195 MB, and the extra random mapping cost 8–10% of simulation
throughput.
The inline table avoids that second lookup. At runtime MAGPIE unpacks the
KLV2 base values and applies the additive KLV3 adjustment in a flat
fixed-size subset pass; exact exchanges and final move equities remain exact.
Legacy RITs remain readable, but leave-derived fields are never trusted for
KLV3 without matching base and contextual fingerprints.

KLV3 can also use a slim, word-only RIT. It preserves the structural word
facts used by move generation—playthrough unions, multi-playthrough
bitvectors, nonplaythrough word-length existence, inline bingos, and the
collision key—but omits all packed leaves, best-leave values, exchanges, and
other KLV-derived fields. Build one by projecting an existing full RIT:

```bash
./bin/magpie convert rit2wordrit CSW24,CSW24_word \
  -threads 10 -savesettings false
```

When exact contextual evaluation is active, MAGPIE prefers
`data/lexica/<lexicon>_word.rit` when that file exists; ordinary KLV2 play
continues to use the full `<lexicon>.rit`. The optional hybrid evaluator uses
the full RIT because positions that fall back to the embedded KLV2 can safely
consume its rack-keyed leave data. The CSW24 word-only table is 426,341,936
bytes (407 MiB), versus 1,885,416,048 bytes (1.8 GiB) for the full RIT, and
took 1.9 seconds to project on the development machine.

The prototype applies KLV3 in move generation, including placements,
exchanges, shadow bounds, and WMP's per-position leave maxima. APIs that are
defined as rack-only lookups, such as `klv_get_leave_value`, deliberately
continue to return the embedded KLV2 baseline because they do not receive an
unseen-pool context. Callers outside move generation that want contextual
values will need an explicit position-aware API.

## Training a CSW24 model

Build a no-PGO binary and record static-autoplay full-rack observations:

```bash
make BUILD=no_pgo_release magpie
mkdir -p obj/klv3-training-csw24
cd obj/klv3-training-csw24
ln -s ../../data data
../../bin/magpie autoplay fj 50000 -lex CSW24 -threads 10 \
  -seed 20260725 -pfreq 1000000 -rit false
```

Then project the observed racks into contextual subleave examples and export
the KLV3:

```bash
python3 ../../tools/train_klv3.py \
  --fj-glob 'autoplay_record_fj_*' \
  --letter-distribution data/letterdistributions/english.csv \
  --base-klv2 data/lexica/CSW24.klv2 \
  --output-prefix data/lexica/CSW24_klv3_ctx
```

The trainer follows the important semantics of standard leavegen: each
observed full rack supplies its best static equity, and that observation is
projected onto sampled tile submultisets. For a retained leave `L` from rack
`R`, it reconstructs the source public pool as the position's public unseen
pool plus `R-L`. Tile-subset sampling naturally gives repeated letters the
correct multiplicity.

The current trainer is a residual fit over an already-converged CSW24 KLV2,
not a replacement for the standard six-generation KLV2 process:

```text
100,200,500,1000,1000,1000
```

The explicit `-rit false` makes corpus generation independent of persisted
settings. Unlike iterative leavegen, this autoplay corpus uses a frozen KLV2,
but disabling RIT also guarantees that its recorded static equities come from
the live KLV rather than cached leave-derived fields. Standard leavegen must
likewise force RIT off because its KLV changes after every generation.

By default it exports only the centered held-tile/unseen-tile interaction
term, at 4x fitted scale. Pool/phase biases and the full uncentered model fit
the held-out regression target slightly better but lost self-play, consistent
with duplicating or confounding main effects already captured by KLV2.

## Prototype results

The training corpus contained 50,000 CSW24 static games and 977,465 full-rack
rows. Deterministic subleave projection produced 1,558,560 examples.

The selected interaction-only model was tested against `CSW24.klv2` in
250,000 paired static games (500,000 games total):

| Metric | KLV3 interaction model | KLV2 |
| --- | ---: | ---: |
| Wins | 249,823 | 248,234 |
| Ties | 1,943 | 1,943 |
| Mean score | 458.859858 | 458.417760 |
| Score difference | **+0.442098/game** | — |

That is +1,589 net wins, or roughly +1.1 Elo. Two earlier independent
50,000-pair rounds at the same scale also had positive score margins. This is
a strong prototype result, but it should still be replicated with a separately
trained corpus before treating the strength estimate as settled.

A zero-coefficient KLV3 produced bit-identical games to KLV2. The focused KLV
and move-generation tests pass, as does the autoplay suite, including
fixed-seed equality across 1 through 11 threads.

The context path's current cost was measured with no-PGO release builds,
10 threads, and 100,000 static games per run. A zero-coefficient KLV3 was
about 3.9% slower by CPU time and 5.2% slower by wall time than KLV2. Because
the games were bit-identical, this isolates the RIT/cache-off runtime path;
the actual contextual arithmetic is precomputed once per position.

The generated development artifact was
`data/lexica/CSW24_klv3_ctx400.klv3`, SHA-256
`974d8a53cbbf75a0fe0d3adbb4c89e13d57c703043fe1cf45a708bffee372df6`.
Generated lexicon data is ignored by git and is not part of the source change.

## Contextual RIT cap experiment

The v2 cap calibration used 1,558,560 projected rows:

- empirical row coverage: 99.311865%;
- 6,087 individually calibrated leaves;
- 158,823 sparse leaves using draw-head fallbacks;
- 749,714 unseen leaves using the model-wide fallback;
- cap range: +0.101 to +22.191, median +4.696.

The inline contextual RIT takes about 66 seconds to create at 10 threads when
an existing CSW24 RIT is supplied as the structural base. Its logical size is
1.8 GB. Because the capped fields occur in every entry, APFS eventually
copy-on-writes most of the base table; the compact out-of-line representation
used less disk but was materially slower.

Performance was measured with no-PGO release builds, 10 simulation threads,
RIT mmap enabled, and exact-vs-p99 configs interleaved within each
matched-position run. Each of nine rounds used 20 positions and three
0.1-second nominations per config. The candidate/exact throughput ratios
ranged from 0.9649 to 0.9814; the mean of the middle three was **0.9728**
(-2.72%). A separate 50-position round measured 0.9558.

A limit-driven 50-position check (three nominations per config) found no
disagreement on positions where both configs repeated the same nomination.
The time-limited runs produced only one such disagreement across the repeated
20-position rounds, and it did not reproduce consistently. This is not
evidence of an oracle-quality gain.

The current conclusion is negative: p99 bounds are safe enough to be
interesting experimentally, but they are looser than the exact
current-position maxima MAGPIE already computes, and the resulting extra
shadow work costs roughly 3% overall. Do not make the capped RIT the default
without a lazy KLV3 path that can skip exact subleave evaluation altogether;
that is where the cap could pay for itself.

The word-only RIT is useful independently of that negative cap result. In the
same no-PGO, 10-thread, mmap-enabled simulation protocol, nine interleaved
20-position rounds gave a warm-cache KLV3/KLV2 throughput ratio of 0.8112
(mean of the middle three ratios). Using the full RIT only for its word facts
gave 0.8102. The relative +0.12% difference is below the benchmark's roughly
1% noise floor, so the slim format is a footprint and startup-I/O
optimization, not a measured simulation-throughput optimization.

## Sparse and hybrid evaluation

Move generation can only produce leaves that are subracks of the current
rack. Computing exact contextual adjustments for only the distinct tile
types on that rack, rather than all alphabet tile types, is bit-identical and
improved the warm-cache KLV3/KLV2 simulation-throughput ratio from 0.8112 to
**0.8296**. This exact sparse evaluation is always enabled.

The experimental hybrid evaluator first computes a cheap position-level
signal: at a representative draw count of four (or the remaining bag size
when smaller), it sums the absolute contextual adjustment of each tile on the
current rack. This samples only one model row per distinct rack tile and does
not enumerate subracks. If the magnitude is at or below the configured
threshold, move generation uses the KLV3 file's embedded KLV2 baseline and
the full RIT's rack-keyed leave fields. Otherwise it performs exact sparse
KLV3 evaluation.

Enable the hybrid with an equity-point threshold:

```bash
./bin/magpie ... -klv3fallback 1.5
```

Use `-klv3fallback -1` to disable it. The sample is a heuristic, not a bound:
the adjustment can vary by draw count and by which subrack a move retains.
Consequently the threshold is an explicit speed/quality tradeoff and is not
enabled by default.

At threshold 1.5, the standard nine-round protocol measured a KLV3/KLV2
throughput ratio of **0.9168**, versus 0.8296 for always-exact sparse KLV3.
Thus this setting recovered about 51% of the remaining KLV2-to-KLV3
throughput gap and left KLV3 about 8.3% slower than KLV2. A threshold high
enough to force every position onto KLV2 measured 1.0013 times ordinary KLV2,
showing that the cheap screen itself is below the benchmark's noise floor.

A deterministic 500-position equal-work screen found 13 root nominee changes
(2.6%) between always-exact KLV3 and threshold 1.5. Each disagreement was
then adjudicated with 500 paired terminal samples under both exact-KLV3 and
hybrid continuation policies. Both policies agreed on the sign in all 13
positions; nine favored the hybrid nominee and four favored the exact
nominee. The policy-ensemble means were +1.386 spread and +0.00546 win
probability per disagreement for the hybrid, but only +0.000118 utility per
screened position with position-level SEM 0.000162. This pilot therefore
found no evidence of a quality loss, but the apparent positive result is not
statistically resolved. Threshold 1.5 should remain experimental pending an
independent, larger position corpus.

## Simulation policy and candidate selection

KLV3's static-game improvement did not carry over when it was used throughout
two-ply simulation rollouts. An equal-work experiment gave KLV2 and KLV3
exactly 1,500 samples per actual candidate arm and evaluated every differing
nomination immediately with a policy-neutral nested oracle. Each policy used
its own leave evaluator at the truncated horizon, avoiding the earlier bug
where KLV3 rollouts ended with a KLV2 residual.

Across 13,074 positions, the policies disagreed 500 times (3.82%). KLV3's
conditional utility delta was -0.000420 with SEM 0.000284 (two-sided p about
0.14); the all-position delta was -0.0000160. The result does not establish
that KLV3 is a worse rollout policy, but it rejects any large benefit at this
sample size. KLV3 also ran 16.5% fewer iterations per second in this matched
protocol, so always-contextual rollouts are not currently justified.

KLV3 was substantially more promising as a static candidate selector for an
otherwise KLV2 simulation. A follow-up generated the top 15 independently
under KLV2 and KLV3, removed the shared plays, simulated only each selector's
unique fringe with the same KLV2 policy and fixed sample count, and sent the
two resulting nominees to the same nested oracle. The top-15 sets fully
overlapped in 849 of 1,349 source positions. In the 500 non-overlap
comparisons, KLV3's candidate won 283-217 (exact two-sided p = 0.00361) and
reduced conditional regret by 0.002856 utility (SEM 0.000723, p = 0.000089).
Assigning zero marginal difference to full-overlap positions gives
+0.001059 utility per source position.

This is deliberately a marginal-candidate experiment, not an end-to-end
strength estimate: a shared candidate discarded by the protocol could beat
both unique nominees. It supports a full-system test in which KLV3 selects the
top 15, KLV2 performs all rollouts, and the actual final nominees are
oracled—including zero difference when both systems choose the same move.

## Next validation steps

- Generate the training corpus from an independent seed and confirm the fitted
  interactions and self-play result.
- Integrate contextual fitting into each generation of the standard leavegen
  loop instead of fitting only after a converged KLV2.
- Run the full-system KLV3-top-15/KLV2-rollout candidate-selection experiment
  justified by the positive marginal-candidate result.
- Replicate the hybrid threshold experiment on an independent position corpus
  and compare thresholds around 1.0 to 1.5 with more oracle samples.
- Revisit lazy exact KLV3 evaluation only if it can avoid evaluating contextual
  subleaves that never survive move-generation pruning. The p99 capped-RIT
  experiment alone did not accomplish this.
- Train and validate models for other distributions and lexica.
