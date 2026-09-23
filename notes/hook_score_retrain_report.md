# Hook-score retrain against `pat_ridgefix_champion_v5`

Research log, 2026-09-22/23. CSW21 only. The original incumbent was not
replaced. Its SHA-256 was saved in `/tmp/hookscore_incumbent.sha` before fitting.

## Current result

The confirmed experimental strength champion is `data/strategy/hookscore_x.pat`
(SHA-256 `e38d86924a754bd181cf64a5913affc4d826640cb38de0006d9a0fc25dbcde8e`).
It beat the original incumbent directly by **+0.4361 per mirrored pair**
on an untouched 1M-pair seed, 95% CI **[0.2782, 0.5940]**. It also
beat the earlier confirmed hook-score C by +0.2317 on a different
untouched 1M-pair seed, CI [0.0743, 0.3892]. C beat the original
incumbent by +0.1714 on its own untouched 1M-pair seed,
CI [0.0591, 0.2837].
The strength gain costs roughly 8% whole-engine throughput versus C,
and 11% versus the incumbent on interleaved local trials. Z, a doubled-
data successor, did not clear its untouched confirmation against X.
The incumbent was not replaced. The subsequent opening-table
refinement and score ablation are documented below.

## Baseline and solver correction

`make magpie BUILD=no_pgo_release`, `make magpie_test BUILD=no_pgo_release`,
and `./bin/magpie_test pat` passed before fitting. The pre-existing uncommitted
variance-scaled ridge code added `(ridge_lambda + shrink_lambda) * N * variance`
to the diagonal but added `shrink_lambda * N * loaded` to the right-hand side.
That inconsistency makes shrinkage on a low-variance feature pull far beyond
the loaded weight. I changed the latter to `shrink_lambda * N * variance *
loaded` and added `test_pat_gen_shrink_variance_scale`; the PAT suite passed.
The production ridge strength and channel defaults remain unchanged.

## Step 1: empirical channel scale

The on-demand `pathookdiag` test used 10,000 seeded CSW21 incumbent self-play
games and recorded 200,874 post-move feature rows with the same extraction
function used by patgen. Ridge lambda is 1. Each old flat diagonal addition
was 200,874; each new addition is `N * max(variance, 1e-6)`.

| d | hook mean | hook var | score mean | score var | corr | old hook/score penalty | new hook penalty | new score penalty |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1.230 | 24.351 | 1.993 | 66.307 | 0.964 | 200,874 | 4,891,530 | 13,319,358 |
| 2 | 4.039 | 92.158 | 3.740 | 86.511 | 0.969 | 200,874 | 18,512,242 | 17,377,849 |
| 3 | 4.758 | 100.554 | 4.626 | 101.891 | 0.961 | 200,874 | 20,198,637 | 20,467,238 |
| 4 | 5.745 | 125.754 | 6.963 | 229.324 | 0.939 | 200,874 | 25,260,794 | 46,065,166 |
| 5 | 5.395 | 127.221 | 5.470 | 142.284 | 0.951 | 200,874 | 25,555,387 | 28,581,128 |
| 6 | 5.225 | 116.756 | 5.108 | 115.992 | 0.962 | 200,874 | 23,453,305 | 23,299,796 |
| 7 | 5.193 | 114.633 | 5.146 | 118.681 | 0.964 | 200,874 | 23,026,708 | 23,839,929 |

The channels are highly collinear. The score variance exceeds the count
variance substantially at d1 and d4, where flat ridge relatively favors
the score channel. Several other pairs have nearly equal variance, so the
scale hypothesis explains only part of the earlier collapse. The
variance-scaled B fit below retains mass in both families.

Run: `./bin/magpie_test pathookdiag` (on-demand; not in normal test suite).

## Candidates and development screens

Input files were copies of `pat_ridgefix_champion_v5.pat`, with option rows
changed only for fitting. Each fitted runtime file has `fit_residual,0`.
All patgen and autoplay commands used `-lex CSW21 -gp true -threads 10
-wmp true -rit true -ritmmap true -wit true`.

| Candidate | Training command (following common flags) | Fit MSE | Held-out MSE | Development screen vs incumbent, 500K mirrored pairs, seed 92300123 |
|---|---|---:|---:|---|
| A: hook-score residual | `patgen 150000 hookscore_a -seed 912300101 -pat hookscore_a_input` | 548.2655 | 549.3269 | +0.1331, SE 0.0637, CI [0.0083, 0.2579] |
| B: both hook families residual | `patgen 150000 hookscore_b -seed 912300102 -pat hookscore_b_input` | 547.6699 | 545.7907 | +0.1291, SE 0.0585, CI [0.0145, 0.2437] |
| B shrink 1 | `patgen 150000 hookscore_bs -seed 912300102 -pat hookscore_bs_input` with `fit_shrink,1` | 547.7578 | 545.8889 | +0.0774, SE 0.0499, CI [-0.0203, 0.1751] |
| B shrink 10 | same run, shrink lambda 10 | 547.8824 | 546.0272 | +0.0551, SE 0.0322, CI [-0.0080, 0.1181] |
| B shrink 100 | same run, shrink lambda 100 | 547.9469 | 546.1134 | Same semantic weights as incumbent; no game screen |
| A, second training seed | `patgen 150000 hookscore_a2 -seed 912300201 -pat hookscore_a_input` | 547.8631 | 544.9161 | +0.1079, SE 0.0620, CI [-0.0136, 0.2293] |
| B, second training seed | `patgen 150000 hookscore_b2 -seed 912300202 -pat hookscore_b_input` | 547.9459 | 547.1893 | +0.1392, SE 0.0586, CI [0.0245, 0.2540] |
| B2 second residual update | `patgen 150000 hookscore_b2_iter -seed 912300303 -pat hookscore_b2_iter_input` | 548.2289 | 546.2475 | +0.1250, SE 0.0602, CI [0.0071, 0.2430] |

Screen command (substitute each candidate name):

```sh
./bin/magpie autoplay games 500000 -lex CSW21 -gp true -threads 10 \
  -seed 92300123 -wmp true -rit true -ritmmap true -wit true \
  -pat1 CANDIDATE -pat2 pat_ridgefix_champion_v5
```

Fitted count-hook / hook-score weights, d1 through d7, in milli-equity:

| Candidate | `hook_d1..d7` | `hook_score_d1..d7` | Collapse? |
|---|---|---|---|
| Incumbent | -47,-28,-22,-21,-21,-19,-16 | 0,0,0,0,0,0,0 | no score fit |
| A | incumbent unchanged | -20,-9,-3,-8,-5,-8,-3 | no |
| B and shrink 0 | -24,-14,-11,-8,-9,-11,-7 | -26,-19,-12,-15,-13,-15,-10 | no |
| B shrink 1 | -37,-22,-17,-15,-15,-16,-12 | -15,-11,-7,-8,-7,-8,-5 | no |
| B shrink 10 | -46,-27,-22,-20,-20,-19,-16 | -3,-3,-2,-2,-2,-2,-1 | no |
| B shrink 100 | incumbent unchanged | 0,0,0,0,0,0,0 | no |
| A, second seed | incumbent unchanged | -19,-10,-3,-7,-4,-8,-2 | no |
| B, second seed | -27,-14,-11,-8,-9,-10,-7 | -28,-19,-12,-15,-13,-14,-10 | no |
| B2 second update | -26,-14,-11,-7,-9,-11,-8 | -28,-20,-13,-15,-13,-14,-11 | no |

The shrink run wrote shrink 0, 1, 10, 100, 1,000, 10,000, 100,000,
and infinity files; the large-strength files return to the incumbent and
are not useful challengers. Held-out MSE is the installed-weight metric
with intercept refit. MSE from A and B are on different self-play samples,
so their absolute values should not be compared directly.

## Full-fit candidate C

`fit_residual,5` explicitly frees the hook-score channels in a full fit;
mode 0 still fixes them. `test/pat_build_hookscore.sh` follows the four-seed
v5 recipe with the 150K through refit, a 500K-pair seed tournament, and a
1,000-rack opening simulation. All four seeds kept both hook families nonzero (d1 count/score: seed 1
-32/-32, seed 2 -33/-30, seed 3 -25/-27, seed 4 -28/-29). In the
500K-pair tournament against seed 1, seed 2 was -0.1264 (SE 0.0604,
CI [-0.2449, -0.0080]), seed 3 was -0.1889 (SE 0.0648, CI
[-0.3160, -0.0618]), and seed 4 was -0.0106 (SE 0.0668, CI
[-0.1415, 0.1204]); seed 1 won. Its final v4 through-refit fit MSE was
545.9394 and held-out MSE 546.1177. The 1,000-rack opening simulation completed: the dev table changed 49 of
500 held-out opening choices and improved paired simulated equity by
+0.190 ± 0.054 per rack. Its output rows are installed in
`hookscore_c.pat`. The whole-game 500K-pair development screen vs the incumbent (seed
92300123) was +0.1620, SE 0.0808, CI [0.0037, 0.3203]. A direct 1M-pair match vs B2 on development seed 92300223 was +0.1010,
SE 0.0473, CI [0.0083, 0.1938] for C. Simulation is not the
acceptance test. The exact C file was frozen before a 1M-pair confirmation
vs the incumbent on untouched seed 923099991.

The experimental builder repeats the full-fit procedures with
these invocations; all internally use CSW21, game pairs, ten threads,
and the four speed-table flags stated above:

```sh
test/pat_build_hookscore.sh hookscore_c /tmp/hookscore_c_build 61000002 777100000 0
test/pat_build_hookscore.sh hookscore_d /tmp/hookscore_d_build 61001002 777200000 0
test/pat_build_hookscore.sh hookscore_e /tmp/hookscore_e_build 61002002 777300000 0
test/pat_build_hookscore.sh hookscore_x /tmp/hookscore_x_build 61003002 777400000 1
test/pat_build_hookscore.sh hookscore_y /tmp/hookscore_y_build 61004002 777500000 1
test/pat_build_hookscore.sh hookscore_z /tmp/hookscore_z_build 61005002 777600000 1 60000 300000
```

The on-demand opening simulator generates its own run seed, so these
commands reproduce the procedure rather than guaranteeing byte-identical
opening rows. The evaluated final files were frozen and verified by hash.

## Untouched-seed confirmation, absolute strength, and speed

C was frozen before confirmation (SHA-256
`a491a62f2af745daf92ac9a1ffbd313dec3f438fdceeb2d26654170bb9f78148`).
On untouched seed 923099991, `autoplay games 1000000` with C as player 1
and `pat_ridgefix_champion_v5` as player 2 yielded **+0.1714 spread per
mirrored pair**, SE 0.0573, 95% CI **[0.0591, 0.2837]**. The frozen file's
hash matched after the match. This satisfies the predeclared win rule.

```sh
./bin/magpie autoplay games 1000000 -lex CSW21 -gp true -threads 10 \
  -seed 923099991 -wmp true -rit true -ritmmap true -wit true \
  -pat1 hookscore_c -pat2 pat_ridgefix_champion_v5
```

C vs no PAT on fresh seed 923099992 (500K pairs) was +3.8435,
SE 0.1334, 95% CI [3.5819, 4.1050]. The incumbent on the same seed was
+3.6313, SE 0.1323, CI [3.3720, 3.8907]. The difference of these two
separate estimates is +0.2122; use the direct mirrored match above for
its valid paired confidence interval. Incumbent provenance: +3.6363, SE
0.1321, CI [3.3774, 3.8952] per 500K mirrored pairs on seed 777100099.
Speed: three interleaved static autoplay runs per file, `games 100000`
(200,000 actual games) with both players loading the same PAT, seed
923055002, 10 threads and all speed tables. Wall seconds and median
throughput:

| File | Seconds, runs 1/2/3 | Turns | Median games/s | Median turns/s |
|---|---|---:|---:|---:|
| v3 no-hook-score control | 46.48 / 41.99 / 41.94 | 4,533,574 | 4,763.0 | 107,968 |
| Incumbent, zero hook-score rows | 42.08 / 42.39 / 42.19 | 4,533,574 | 4,740.5 | 107,456 |
| C, active hook-score rows | 43.00 / 42.81 / 42.82 | 4,544,622 | 4,670.7 | 106,133 |

The first v3 run was a cold outlier, so the median is used. The v3
no-channel control has the incumbent's other weights and options and
played identically to it over 1,000 mirrored pairs (0.0000 paired
spread and SE). Incumbent versus v3 control shows no measurable
flag-off regression at this precision (median incumbent games/s is
0.47% lower amid run-order noise). Active C is 1.47% lower in games/s
and 1.23% lower in turns/s than incumbent; some game-rate difference
reflects C's 0.24% greater turn count. This is a whole-engine throughput
measurement, not an isolated PAT scanner microbenchmark.

```sh
/usr/bin/time -p ./bin/magpie autoplay games 100000 -lex CSW21 \
  -gp true -threads 10 -seed 923055002 -wmp true -rit true \
  -ritmmap true -wit true -pat1 MODEL -pat2 MODEL
```

## Files and hunks added by this work

- `src/impl/pat_gen.c` and `src/impl/pat_gen.h`: scale the shrinkage
  right-hand side by empirical feature variance, update its documented
  formula, and let explicit mode 5 free hook-score channels while bypassing
  residual-only selection.
- `src/ent/pat.c`: accept `fit_residual,5` in the file parser; gate
  hook-score exposure calculations when runtime score weights are all zero,
  while keeping training-row extraction fully enabled.
- `src/ent/pat.h`: reuse the existing `lm_channels` context byte as channel
  flags for LM and hook-score scan work (no context-size growth).
- `src/def/pat_defs.h`: document mode 5 and clarify that mode 0
  fixes the experimental hook-score rows.
- `test/pat_test.c`: regression test for variance-scaled shrinkage;
  on-demand real self-play diagnostic; mode-5 solver coverage.
- `test/pat_test.h` and `test/test.c`: declare and register `pathookdiag`.
- `test/pat_build_hookscore.sh`: experimental full v5 build script,
  with optional exact-hook and training-sample arguments.
- `notes/hook_score_retrain_report.md`: this report.

The pre-existing uncommitted files and hunks were left in place. All weight
files are untracked, gitignored data in `data/strategy/`.

## Current recommendation

X is the current strength champion: its frozen exact-hook full-fit file
beat the prior confirmed C on an untouched 1M-pair seed (details below).
C itself beat the original incumbent on its own untouched 1M-pair seed.
Keep both as experimental artifacts. X also beat the original
incumbent directly but pays a measurable whole-engine speed cost.
Recommend no shipping in this task, as requested. For a later promotion
decision, X is the strongest CSW21 choice if its roughly 11% throughput
cost is acceptable; C is a faster, still validated improvement over
the incumbent. The evidence covers CSW21 static players on this
Apple M4; other lexica and simming players were outside this task.
At this intermediate stage, no file had been installed, committed, or pushed. The next useful work is
to profile exact created-hook evaluation, especially the
`pat_fresh_cross_set` GADDAG traversal suggested by code inspection,
before considering caching. Rerun the flag-off and active speed
controls with game-output parity after any optimization. Any model
for another lexicon needs its own training and untouched confirmation.

## Follow-on hook-score strength sweep (development only)

After C met the untouched-seed win criterion, three copies scaled only its
seven `hook_score_d*` weights. Against frozen C on development seed
92300323, 500K mirrored pairs each:

| Score multiplier | Mean per pair | SE | 95% CI |
|---:|---:|---:|---:|
| 0.50 | +0.0366 | 0.0642 | [-0.0892, 0.1624] |
| 0.75 | +0.0715 | 0.0454 | [-0.0174, 0.1604] |
| 1.25 | -0.0094 | 0.0470 | [-0.1016, 0.0828] |
| 1.50 | -0.0910 | 0.0627 | [-0.2138, 0.0319] |

Command shape: `autoplay games 500000 -seed 92300323 -pat1
hookscore_c_scoreNNN -pat2 hookscore_c`, with the common CSW21,
game-pair, thread, and speed-table flags above. On independent development seed 92300423, 0.75 lost to C by
-0.0649, SE 0.0322, CI [-0.1281, -0.0018] over 1M pairs. The first
seed's positive estimate did not replicate; none of these strength
variants replaces confirmed C. No confirmation seed was spent on them. These variants
have not been promoted or tested on the confirmation seed.

## Follow-on residual refit on C

A fresh 150K-game `fit_residual,2` self-play run from C (seed 912300404),
with `fit_shrink,1`, changed the hook weights by at most two milli-equity
in the unshrunk candidate. The unshrunk fit MSE was 548.0492; held-out
MSE 547.6320 versus 547.6185 for loaded C. Whole-game screen against C
on seed 92300523, 500K pairs: +0.0059, SE 0.0254, CI
[-0.0438, 0.0556]. This line is null; high-shrink candidates return to
C and were not screened.

Afterward, the zero-hook-score scanner was gated. `./bin/magpie_test pat`
passed, and a 10K-pair C versus incumbent game summary on seed 923055003
was byte-for-byte identical before and after the gate. The earlier speed
table above is pre-gate. Repeating the same 200,000-game benchmark after
the gate gave:

| File | Seconds, runs 1/2/3 | Turns | Median games/s | Median turns/s |
|---|---|---:|---:|---:|
| v3 no-hook-score control | 40.78 / 40.69 / 40.59 | 4,533,574 | 4,915.2 | 111,417 |
| Incumbent, zero hook-score rows | 41.66 / 40.98 / 40.72 | 4,533,574 | 4,880.4 | 110,629 |
| C, active hook-score rows | 42.77 / 42.92 / 43.25 | 4,544,622 | 4,660.8 | 105,886 |

The incumbent's zero-score path improved 2.95% in median games/s versus
its pre-gate build, with identical game output. The v3 control and
explicit-zero incumbent remain within 0.71% amid run-order drift, so no
measurable file-format off-path cost is established. Active C costs 4.52%
in games/s or 4.29% in turns/s versus the now-gated incumbent. The two
models play slightly different game lengths; turns/s is the cleaner
whole-engine throughput comparison. These timings are on this M4 and
include the whole engine; they do not isolate a PAT routine.

## Build and static checks

After the scanner gate, `make magpie BUILD=no_pgo_release`, `make
magpie_test BUILD=no_pgo_release`, and `./bin/magpie_test pat` passed.
`python3 format.py --write` was run on every C file touched by this work,
and `git diff --check` passed. `sh -n test/pat_build_hookscore.sh` passed.

`./cppcheck.sh` built and ran cppcheck 2.17.1 but exited 1 on diagnostics
from the pre-existing tree, including `test/pat_test.c:975`'s
`key`-may-be-uninitialized warning and style findings in files this task
did not change. The diagnostic test's `key` array is populated from
fixed strings of length 1–3 before use; no new finding was reported in
the scanner gate. The temporary cppcheck build was removed afterward.
`./tidy.sh` could not run analysis because `clang-tidy` is not installed
(exit 127 for each attempted file). `python3 find_circ_deps.py` could
not run because the local Python lacks `networkx` (`ModuleNotFoundError`).

## Second independent full-fit batch

A second four-seed full-fit v5 batch, `hookscore_d`, uses training seeds
61001002–61001005 and tournament seeds 777200002–777200004. It follows
the same separate script and opening-table procedure, leaving C and the
incumbent untouched. Its winner was screened directly against C. Internal 500K-pair
tournament results: seed 2 vs seed 1, -0.0759 (SE 0.0598, CI [-0.1932, 0.0413]); seed 3 vs seed 1,
+0.1352 (SE 0.0643, CI [0.0092, 0.2611]). Seed 4 vs seed 1 was +0.1399 (SE 0.0580, CI [0.0263, 0.2535]); seed 4 won by point estimate. Its 1,000-rack opening simulation completed: the dev table changed 43
of 500 held-out choices and improved paired simulated equity by
+0.226 ± 0.061 per rack. D uses seed 4's through-refit weights:
`hook_d1..d7` = -30,-17,-11,-10,-11,-10,-7 and
`hook_score_d1..d7` = -30,-22,-13,-16,-15,-14,-10, with fit MSE
546.4812 and held-out MSE 546.0888. Its 500K-pair direct development screen against C on seed 92300623
was -0.1985, SE 0.0667, CI [-0.3294, -0.0677]. D is rejected; the
internal tournament did not predict a win over C.

## Intermediate-file cleanup

After rejecting D and the score-strength and residual variants, 115
nonfinal experimental `.pat` files from batches A–D and the v3 speed
control were removed. Their fit reports and compact command logs remain
for audit. The confirmed `data/strategy/hookscore_c.pat` retained its frozen
SHA-256. Further nonfinal E and F files were removed after their
screens; later cleanup is summarized at the end of this report.

## Third independent full-fit batch

Batch E uses training seeds 61002002–61002005 and internal tournament
seeds 777300002–777300004, following the same four-seed full v5 script.
All four iterative and through-refit stages completed. Tournament results, 500K pairs each versus seed 1: seed 2
+0.0026 (SE 0.0550, CI [-0.1052, 0.1104]); seed 3 +0.0082
(SE 0.0567, CI [-0.1029, 0.1193]). Seed 4 was +0.0811
(SE 0.0650, CI [-0.0463, 0.2084]) and won by point estimate. Its 1,000-rack opening simulation completed: the dev table changed 47
of 500 held-out choices, improving paired simulated equity by
+0.189 ± 0.058 per rack. E's final `hook_d1..d7` =
-27,-17,-12,-9,-10,-10,-6 and `hook_score_d1..d7` =
-28,-23,-15,-15,-13,-14,-9; through-refit fit MSE 546.4289,
held-out MSE 545.4679. Direct E-versus-C play on seed 92300823 was -0.0526,
SE 0.0672, CI [-0.1844, 0.0791] over 500K pairs. E is not promoted;
At that stage, C remained the confirmed champion.

## Full-vector adaptation from C

A 150K-game self-play fit from C on seed 912300505 used explicit
`fit_residual,5` (all regular channels including hook-score free) and
`fit_shrink,1`. Held-out installed-weight MSE was 547.0548 for loaded C;
shrink 0/1/10 yielded 546.9866/546.9774/547.0356 with intercept
refit. These predictive gains do not establish playing strength. The
unshrunk candidate changed 55 weights from C, shrink 1 changed 50, and
shrink 10 changed 26. Their whole-game screens vs C, 500K mirrored pairs on development
seed 92301023, were: shrink 0 -0.1465 (SE 0.0693, CI
[-0.2824, -0.0107]); shrink 1 -0.1160 (SE 0.0625, CI
[-0.2384, 0.0065]); shrink 10 -0.0683 (SE 0.0289, CI
[-0.1251, -0.0116]). All trailed C despite predictive MSE gains. Shrink 100 and above were
nearly identical to C and were not screened.

## Exact created hooks on C

The existing opt-in `exact_created_hooks,1` resolves new perpendicular
cross sets on the GADDAG instead of the default approximation. A copy
of C with only that option row changed beat C on development seed
92301123 by +0.4252 per mirrored pair over 500K pairs, SE 0.1163,
95% CI [0.1972, 0.6532]. This is particularly relevant to the
now-nonzero score-weighted hook channels; C itself remains unchanged.
The exact file was frozen (SHA-256
`91df37459d751c9d06db71ba73be41390a39829e8551a206c7068764e1ceaaa9`).
A 1M-pair direct replication vs C on second development seed 92301223
was +0.1661, SE 0.0824, CI [0.0047, 0.3276]. The frozen file's hash
matched afterward. A separate untouched-seed 1M-pair confirmation vs C on seed
923099994 was +0.1020, SE 0.0823, CI [-0.0593, 0.2633]. Its 95%
lower bound is below zero, so it does not meet the predeclared win rule
and is not promoted. The frozen SHA-256 still matched. At that stage, C remained the
confirmed champion.

## Full retrain with exact created hooks

The exact flag on C improved two development matches but failed the
predeclared untouched 1M-pair lower-bound rule. Batch X is a distinct
candidate: four full-fit seeds trained from zero with both
`fit_residual,5` and `exact_created_hooks,1`, training seeds
61003002–61003005 and internal tournament seeds 777400002–777400004.
The 500K-pair tournament against seed 1 gave seed 2 -0.0249 (SE 0.0671,
CI [-0.1565, 0.1067]), seed 3 -0.0705 (SE 0.0631, CI
[-0.1942, 0.0532]), and seed 4 +0.0003 (SE 0.0581, CI
[-0.1136, 0.1143]). Seed 4 narrowly won by point estimate. Its
1,000-rack opening simulation changed 44 of 500 held-out choices and
improved paired simulated equity by +0.142 ± 0.049 per rack. The final
through-refit fit MSE was 545.7476 and held-out MSE 547.1630. Final
`hook_d1..d7` = -27,-15,-12,-8,-9,-10,-8 and `hook_score_d1..d7`
= -29,-20,-15,-15,-13,-14,-12. The runtime file retains
`exact_created_hooks,1` and has `fit_residual,0`.

X beat C on new development seed 92301323 by +0.3180 per mirrored pair
over 500K pairs, SE 0.1136, CI [0.0953, 0.5408]. The exact file was
frozen (SHA-256 `e38d86924a754bd181cf64a5913affc4d826640cb38de0006d9a0fc25dbcde8e`)
before a separate 1M-pair development replication on seed 92301423.
That match was +0.2094, SE 0.0803, CI [0.0520, 0.3668]. Its frozen
hash matched before an untouched 1M-pair confirmation vs C on seed
923099996. That match was **+0.2317 per mirrored pair**, SE 0.0803,
95% CI **[0.0743, 0.3892]**. The frozen X, C, and incumbent SHA-256
checks all passed afterward. This meets the predeclared win rule and
makes X the new strength champion.

On separate development seed 92301523, X beat the original incumbent
directly by +0.4372 per mirrored pair over 500K pairs, SE 0.1136,
CI [0.2145, 0.6599]. Interleaved 200K-game whole-engine speed trials on seed 923055007,
three runs per file with both players loading the same PAT, gave:

| File | Seconds, runs 1/2/3 | Turns | Median games/s | Median turns/s |
|---|---:|---:|---:|---:|
| Original incumbent | 42.37 / 38.97 / 38.50 | 4,534,192 | 5,132.2 | 116,351 |
| C | 40.49 / 40.82 / 40.47 | 4,545,562 | 4,939.5 | 112,264 |
| X, exact hooks | 44.00 / 44.13 / 43.84 | 4,545,926 | 4,545.5 | 103,317 |

X is 8.0% slower in games/s and 8.0% slower in turns/s than C,
and 11.4% slower in games/s and 11.2% slower in turns/s than the
incumbent on this run. Run-order and ambient load vary, but X's three
samples are consistent. This measures the whole engine, not just PAT.
The production defaults and C's frozen file remain unchanged.

Absolute strength on fresh seed 923099997, 500K mirrored pairs
against no PAT: X +3.6293 (SE 0.1356, CI [3.3635, 3.8951]);
original incumbent on the same seed +3.5226 (SE 0.1324,
CI [3.2630, 3.7822]). The difference of separate estimates is
+0.1067, but their covariance is unavailable; use the direct paired
X-versus-incumbent match for a valid confidence interval. The different
absolute seed also explains why X's raw number should not be compared
directly with C's +3.8435 on seed 923099992.

X versus the earlier C-with-exact-hooks copy on development seed
92301623 was +0.0271 per mirrored pair over 500K pairs, SE 0.0685,
CI [-0.1071, 0.1612]. The result is inconclusive; it suggests that
exact evaluation accounts for much of the measured improvement over C,
but does not establish equivalence. X remains the confirmed champion.

## Independent exact-hook successor batch Y

After the X result, another four-seed full exact-hook retrain used
training seeds 61004002–61004005 and tournament seeds
777500002–777500004. The 500K-pair tournament versus seed 1 gave
seed 2 -0.0712 (SE 0.0514, CI [-0.1720, 0.0296]), seed 3
-0.0973 (SE 0.0563, CI [-0.2076, 0.0131]), and seed 4
-0.0264 (SE 0.0607, CI [-0.1454, 0.0925]); seed 1 won.
Its 1,000-rack opening simulation changed 42 of 500 held-out choices
and improved paired simulated equity by +0.076 ± 0.048 per rack.
Y's through-refit fit MSE was 545.9834 and held-out MSE 545.6777.
Final `hook_d1..d7` = -31,-14,-14,-9,-9,-12,-7 and
`hook_score_d1..d7` = -32,-19,-18,-15,-14,-15,-9. A 500K-pair
direct screen versus X on development seed 92301723 was -0.0190
per mirrored pair, SE 0.0585, CI [-0.1336, 0.0957]. Y is not
promoted; X remains champion. Only nonfinal X intermediate `.pat` files were removed; the frozen
X, C, and original incumbent files remain untouched.

## Doubled-data exact-hook successor batch Z

The experimental builder now accepts optional iterative and through
training game counts; its defaults remain 30K per generation and 150K
through. Batch Z uses 60K per each of five iterative generations and
300K for the through refit, with four seeds 61005002–61005005,
internal tournament seeds 777600002–777600004, and exact created
hooks. The 500K-pair internal tournament versus seed 1 gave seed 2
-0.1742 (SE 0.0520, CI [-0.2763, -0.0722]), seed 3 -0.0703
(SE 0.0485, CI [-0.1653, 0.0247]), and seed 4 -0.0509
(SE 0.0563, CI [-0.1613, 0.0594]); seed 1 won. Its 1,000-rack
opening simulation changed 37 of 500 held-out choices and improved
paired simulated equity by +0.153 ± 0.046 per rack. Through-refit
fit MSE was 546.0625 and held-out MSE 545.5976. Final
`hook_d1..d7` = -30,-14,-12,-8,-11,-11,-6 and
`hook_score_d1..d7` = -31,-19,-13,-15,-15,-14,-9. A direct
500K-pair whole-game screen versus frozen X on development seed
92301823 was +0.0819 per mirrored pair, SE 0.0567, CI
[-0.0293, 0.1931]. Z was frozen (SHA-256
`059745aafa01eb3fe0ce2a6c8fb10eaf4d99a32206f4a3fc89958284fdaec19f`) for a second 1M-pair development
replication on seed 92301923. That match was +0.0857 per mirrored pair,
SE 0.0402, CI [0.0069, 0.1645]. The frozen hash matched before a
fresh untouched 1M-pair confirmation versus X on seed 923099998.
That match was +0.0347 per mirrored pair, SE 0.0402,
CI [-0.0441, 0.1136]. Its lower bound is below zero, so Z fails
the predeclared promotion rule. All frozen X, Z, C, and incumbent
hash checks passed afterward. X remains champion.

## Frozen-fit blend

A 50/50 average of all integer feature rows in frozen X and Z changed
32 weights from X. It keeps X's exact-hook setting, other options, and
opening table. The file `hookscore_blend_xz.pat` was screened
against frozen X over 500K mirrored pairs on development seed
92302023: -0.0165, SE 0.0460, CI [-0.1067, 0.0737]. It does not
improve on X and is rejected.

## Exact-hook score-strength sweep

Two copies of frozen X scale only its seven `hook_score_d*` rows to
75% or 125% (integer milli-equity rounding). Other weights, the exact
hook option, and opening table match X. Each was screened against
X on development seed 92302123 over 500K mirrored pairs. The 75%
variant was +0.0137 (SE 0.0448, CI [-0.0740, 0.1015]); the 125%
variant was -0.1105 (SE 0.0436, CI [-0.1960, -0.0251]). Neither
improves on X; the higher scale clearly hurts.

## Larger opening-table simulation

The X opening table came from 1,000 simulated racks, with 500 held
out by parity. A new 5,000-rack simulation of frozen X used 2,500
held-out odd racks. Its dev table changed only 82 of those choices
(3.3%) and the paired simulated improvement was -0.004 ± 0.013
per rack. That does not support a further correction. The simulator reports
`sim_equity - static_equity` for the PAT file it loaded. Because this
run loaded X with its existing opening rows, the printed rows are a
*residual correction*; replacing X's rows with them would be the
wrong interpretation. A replacement copy was screened as a diagnostic on seed 92302223,
returning -0.0962 per mirrored pair (SE 0.0561, CI
[-0.2061, 0.0138]); its result is not used for the recalibration
decision. A separate additive copy sums X's rows with the printed
residual rows, then subtracts a common maximum to keep all opening
rows nonpositive. That correctly constructed file was screened against X on seed
92302323: +0.0179 per mirrored pair, SE 0.0276, CI
[-0.0362, 0.0720]. It does not establish an improvement and is
not promoted. The verbose simulation log was reduced to a
22-line summary.

## Opening-table ablation

A copy of X set all seven `opening_*` rows to zero, keeping every
feature weight and exact-hook option unchanged. Against X on
development seed 92302423 over 500K mirrored pairs, it was -0.0577
per pair, SE 0.0527, CI [-0.1611, 0.0456]. This does not support
removing X's original opening table.

## Exact-to-approximate weight transfer

A copy of X changes only `exact_created_hooks,1` to `0`, keeping
all fitted weights and opening rows. This tests whether the exact-hook
retrain produces a faster approximate-hook successor to C. Against frozen C on new development seed 92302523 over 500K
mirrored pairs, it was +0.0109, SE 0.0678, CI
[-0.1220, 0.1437]. It does not beat C and is discarded.

## Champion hook-score ablation

A copy of frozen X sets only its seven `hook_score_d*` weights to zero,
keeping exact created hooks, every other feature weight, and opening
rows. Against X on development seed 92302623 over 500K mirrored pairs,
the no-score copy lost by -0.4572 per pair, SE 0.0832,
CI [-0.6203, -0.2940]. This directly supports the score channels'
marginal contribution within X, rather than comparing different
full retrains.

## Direct final confirmation against the original incumbent

The frozen X file and original incumbent passed SHA-256 checks
before a fresh untouched 1M-pair direct match on seed 923099999
(2M actual games; 0 incomplete endings). X won by **+0.4361 per mirrored pair**, SE 0.0806, 95% CI
**[0.2782, 0.5940]**. Using the same reported SE, a normal
99% interval is approximately [0.2285, 0.6437], still wholly above
zero despite the exploratory candidate search.

```sh
./bin/magpie autoplay games 1000000 -lex CSW21 -gp true -threads 10 \
  -seed 923099999 -wmp true -rit true -ritmmap true -wit true \
  -pat1 hookscore_x -pat2 pat_ridgefix_champion_v5
```

The hashes matched again afterward. This independently satisfies the original acceptance rule. The earlier
500K-pair development screen on seed 92301523 was +0.4372,
SE 0.1136, CI [0.2145, 0.6599], closely consistent.

## Final verification and artifact cleanup

After the final PAT header comment edit, `make magpie BUILD=no_pgo_release`,
`make magpie_test BUILD=no_pgo_release`, and `./bin/magpie_test pat`
all passed again. `python3 format.py --write pat_defs.h` reported no
changes needed. `sh -n test/pat_build_hookscore.sh` and
`git diff --check` passed. Frozen SHA-256 checks passed for X, C,
Z, the C-with-exact-hooks copy, and the original incumbent. Roughly 23 GiB remained free on `/`; the retained experiment
logs, reports, and PAT artifacts total about 2.4 MiB.
Nonfinal A–F, Y, blend, score-strength, opening-table, transfer,
score-ablation, and Z intermediate PAT files created during this task
were deleted. Four
small final-round files remain in gitignored `data/strategy/`: the
confirmed X and C files, plus frozen Z and C-with-exact-hooks copies
for audit. No weight file was installed in place of the incumbent.

## Independent complete-game comparison with no PAT

A fresh 500,000 mirrored-pair match on seed 923100123 completed 1,000,000
games with X as player 1 and no PAT as player 2. X won 505,085 games, lost
490,897, and tied 4,018. Its tie-adjusted win rate was **50.7094%**, compared
with 49.2906% for no PAT. The spread advantage was **+3.8427 points per
mirrored pair** (SE 0.1354, 95% CI [3.5773, 4.1081]), or about +1.9214
points per game. This is consistent with the earlier 500,000-pair X versus
no-PAT result (+3.6293 per pair on seed 923099997). All games ended, with
999,514 standard endings and 486 pass endings. The run used CSW21, WMP,
RIT, WIT, ten threads, and the frozen X SHA-256 above.

```sh
./bin/magpie autoplay games 500000 -lex CSW21 -gp true -threads 10 \
  -seed 923100123 -wmp true -rit true -ritmmap true -wit true \
  -pat1 hookscore_x -pat2 none
```
