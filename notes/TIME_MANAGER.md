# TimeManager: value of computation

## Objective

TimeManager should minimize expected game-level utility regret under the
player's real clock. Saving time is not itself a benefit. A turn deposits time
only when the best available additional computation is worth less than the
expected future use of that time; a later turn withdraws it only when the
inequality reverses.

The real game clock remains the bank. TimeManager does not maintain a second
spendable balance that can drift away from it.

## Hardware-independent value curves

Offline calibration uses solver work rather than elapsed time:

- simulation: rollout nodes, stratified by ply depth and game phase;
- endgame: cumulative alpha-beta nodes at each completed IDS depth;
- PEG: cumulative endgame nodes plus completed scenarios, candidates, and
  stage boundaries.

PEG cannot be represented faithfully by endgame nodes alone: greedy seed work
and move generation can consume substantial CPU while producing zero endgame
nodes. Endgame nodes are the primary continuous work coordinate, while
candidate/stage completion identifies the lumpy result boundaries.

Each curve estimates expected oracle regret after work `n`:

```text
R(mode, position_features, n)
```

The value of the next work chunk is the expected decrease in regret. Curves,
node targets, and stage/depth transition models are shared across hardware.
At runtime, a mode-specific online NPS estimate translates work into seconds.
NPS is an execution estimate, not a learned strength feature. The scheduler's
final currency is still seconds: it compares expected regret reduction per
predicted second after translating portable work through the current rate.
In that sense NPS measures available processing power; it is not itself the
thing TimeManager is trying to spend.

The rate estimate must reflect the machine's *available* processing power, not
its advertised or best-ever throughput. Keep separate rolling rates for:

- simulation rollout nodes;
- endgame alpha-beta nodes, conditioned on thread count;
- PEG scenarios and nested-endgame nodes, conditioned on bag size and stage.

Ignore warm-up intervals, use a robust lower estimate rather than a peak, and
downweight intervals whose throughput is unstable. Thermal throttling or
unrelated activity then shrinks allocations automatically. Offline calibration
runs record both wall time and aggregate process CPU time. Process CPU divided
by wall time estimates scheduled cores. On a *repeated identical workload*, a
drop can reveal CPU contention, while a wall-NPS decline at steady scheduled
cores can reveal throttling. Across different positions or solver depths,
either value also reflects legitimate work mix and parallel efficiency, so it
is not a quietness test by itself. Report median rate, robust MAD-based
dispersion, and late-versus-early drift. Reject or repeat rounds with
substantial interference; interleaved A/B runs still need an otherwise quiet
machine. Neither normalization can rescue a benchmark sharing the machine with
a sustained heavy workload.

`tools/analyze_work_rates.py` applies those checks to analysis-trace TSVs. It
keeps PEG scenarios and nested-endgame nodes as separate coordinates instead
of pretending they are interchangeable.

For offline experiments, throughput from arbitrary positions is not by itself
a quietness test: node cost varies with the position. Repeat an identical
sentinel workload before, during, and after a long run, alternate experimental
arm order, and reject a run if sentinel throughput or scheduled-core occupancy
drifts materially. The sentinel is only a measurement control. At runtime,
lower throughput caused by real co-load is not rejected; it is the currently
available processing power and TimeManager should plan against it.

## Deposit and withdrawal rule

At the start of a turn, reserve:

1. overtime/latency safety;
2. a minimum allocation for each plausible remaining turn;
3. enough time to return the best already-known legal move.

The future-turn reserve is phase-aware, not the current allocation multiplied
by the number of turns. In particular, successive endgame searches normally
become much cheaper as tiles disappear. Forecast that declining trajectory
from game state and reserve a conservative quantile of its total cost. If the
clock already covers the safety reserve plus plausible future demand, unused
time has no terminal value and the future shadow price approaches zero. In
that case keep buying positive-value current work, potentially through an
exact result, rather than banking time that is unlikely to be needed.

The remaining clock is discretionary. Define the future shadow price
`lambda` as the expected utility-regret reduction per second available on
future turns under the remaining clock and predicted mode/phase mix.

Take another computation chunk only when:

```text
expected current regret reduction / predicted chunk seconds > lambda
```

and the safety reserve remains intact. Stopping before the old equal slice is a
deposit. Continuing past it is a withdrawal. The equal slice is a comparison
baseline, not a hard boundary.

Chunks align with useful result boundaries:

- sim: a node tranche ending at the next regret-estimator check;
- endgame: the predicted nodes needed to finish the next IDS depth;
- PEG: the predicted nodes needed to finish the current candidate, then the
  current stage. Do not buy half a candidate whose result will be discarded.

Completion is a hard gate independent of value. Before starting an endgame
depth, estimate the conditional distribution of its added nodes from the
completed-depth trace, then convert a one-sided quantile with a conservative
live NPS estimate. Do not start unless that entire bound plus return latency
fits before the hard deadline. "Ample time" lowers the future shadow price; it
does not waive this admission test. The production confidence target should
be chosen explicitly (initially p99) and audited as an out-of-sample
false-start rate.

No bound means no start for an indivisible endgame depth or PEG
candidate/stage. It must not silently fall back to treating expected work as
deterministic. Simulation checkpoints can omit a statistical completion bound
only when they are deliberately fixed, interruptible node tranches.
Likewise, a valid bound below the configured policy confidence is refused;
the initial `TimeManagerClock.minimum_completion_confidence` target is 0.99.

Replan at every completed boundary. Expected work converted with mean live
throughput prices a depth's value; the one-sided work bound converted with a
conservative live-throughput estimate admits it. Do not use the p99 cost as
though it were the expected cost (that undervalues useful search), or the mean
cost/rate as the admission bound (that strands unfinished work). An emergency
hard interrupt still protects the clock from model failure, but an interrupted
depth is an admission failure and should be counted as such; it is not a
normal scheduling strategy.

The advertised completion confidence is joint. A p99 node bound converted at
a separately estimated p99-safe rate is not p99 end to end; without stronger
assumptions its union-bound guarantee is only p98. Either calibrate the final
wall-time residual jointly, or split the failure budget explicitly between
work and throughput (for example 0.5% each for a 1% total target). Audit both
false starts and rejected depths that would actually have fit. With zero
false starts, about 299 independent held-out starts are required merely to put
the one-sided 95% binomial upper bound below 1%, so the current 120-position
corpus can choose models but cannot certify a production p99 gate.

Endgame EBF and PEG candidate timestamps help estimate the next chunk, but a
single EBF multiplier is not sufficient. The observed depth-4-to-5 EBF p95
was 43.8 in one hard corpus and 22.6 in the independent replication; a bound
trained only on the latter covered just 86% of the former. Fit a
depth-specific conditional model using cumulative nodes, the last two depth
increments, root/ply-2 work, and game-state complexity, then conformally
calibrate its upper residual on independent source games. If a boundary
cannot fit, return the last complete answer.

## Calibration status

PR #633 supplies a usable simulation prior at 300K, 1M, 3M, and 10M nodes.
The existing endgame and PEG PR descriptions supply only isolated operating
points:

- PR #625: endgame at 100 ms/root, with nodes/depth and exact disagreement
  scoring;
- PR #625: PEG at 8 s/arm, with candidate/stage progress;
- PR #595: examples showing that PEG value can jump discontinuously when a
  class candidate or deeper stage completes.

Those results demonstrate that extra work can help, but do not estimate a
marginal value curve and therefore cannot price withdrawals.

The endgame calibration harness records `(depth, cumulative nodes, returned
move)` and exact-scores each distinct returned move. The solver also supports
an opt-in aggregate node ceiling so fixed-node arms can be measured directly.
PEG calibration must log both endgame nodes and its structural work units
before a node-budget sweep is meaningful.

`tools/analyze_endgame_value_curve.py` turns those completed-depth events into
regret and marginal-gain curves and reports the observed effective branching
factor distribution. The scheduler can use the latest completed-depth node
increment and a conservative EBF quantile to price the next indivisible depth;
root/ply-2 coverage describes interrupted work but does not make an incomplete
depth equivalent to a completed result. Pass three or more identical-corpus
logs to the analyzer to normalize repeated per-position solve rates and gate
the clock conversion without counting those repeats as extra quality samples.
The harness also accepts a per-position wall cap and reports truncated searches
separately, so the heavy tail can be measured without silently treating an
incomplete depth as oracle truth.

`pegstructcurve` avoids a time-budget confound by completing the greedy seed
and successive halving stages on the same positions, alternating shallow-first
and deep-first arm order, and having one common oracle score every returned
move. Every arm repeats the identical greedy prefix; its separate wall/CPU/work
record is a built-in sentinel for machine interference.
`tools/analyze_peg_structural_curve.py` reports oracle regret at those
boundaries, gates timing fits on the repeated-prefix stability, and fits the
local *post-greedy* conversion

```text
incremental seconds ~= a * added scenarios + b * nested-endgame nodes
```

The already-completed greedy prefix has its own observed
seconds-per-scenario conversion. This prevents its position-specific movegen
cost from being charged again when deciding whether to buy a refinement
stage. The fit is a clock-cost model, not the strength curve. Its arm-order
residual and process-CPU/wall dispersion are noise diagnostics; a noisy run is
repeated rather than normalized into the quality data.

The first quiet calibration checks illustrate why the separation matters:

- Three repeated 10-thread endgame passes over the same 10 hard positions had
  1.48% normalized rate CV, -0.17% early/late drift, and 1.35% normalized
  scheduled-core CV. Raw interval NPS CV was 47% because positions and depths
  had legitimately different work mix.
- With a five-second cap, 8/10 reached depth 8; the two 12-13-tile tails only
  reached depth 4 after about 8.3M nodes. Endgame therefore needs a
  next-completed-depth cost distribution, not just an average NPS.
- A six-position 1-in-bag PEG pilot found the greedy prefix around 0.03s and
  stage 1 around 2.27s median. Added nested-endgame nodes explained most of
  this particular refinement cost, but the sample is far too small to fit a
  production value curve or generalize to larger bags/stages.

### Held-out EG/PEG calibration (2026-07-29)

The endgame evidence now contains three held-out corpora, all run in
`no_pgo_release` with 10 threads. The first has 60 balanced positions, ten at
each combined rack size from 8 through 13. Two independent hard corpora have
60 positions each, 20 at each rack size 12--14 and one position per source
game. Utility below is deterministic win utility plus `1e-4 * spread`;
"reference" means the deepest completed calibration result, not a proof that
a fixed-depth search is the terminal exact solution.

- On the 40 positions with 8--11 rack tiles, 39 reached the depth-10
  reference under the five-second cap. Mean utility regret was 0.106589 at
  depth 1, 0.000073 at depth 2, 0.000030 at depth 3, 0.000005 at depth 4,
  and zero observed at depth 5. Median added work to finish depths 2--5 was
  5,819, 10,255, 9,305, and 24,881 nodes.
- All 20 positions with 12--13 rack tiles reached the depth-8 reference after
  explicitly finishing three censored tails (up to 134.1M nodes). Mean utility
  regret was 0.223728 at depth 1, 0.105553 at depth 2, 0.050150 at depth 3,
  and zero observed at depth 4. Median added work to finish depths 2--5 was
  24,112, 157,068, 141,658, and 455,448 nodes.
- Three identical timing passes were quiet: normalized rate CV was 1.15% for
  rack sizes 8--11 and 1.22% for 12--13, with zero measured early/late drift.
- All 60 positions in the first 12--14 corpus reached the depth-6 reference.
  Fifty-seven finished under the 60-second cap; the three censored tails were
  finished separately, with the largest taking 284.5M cumulative nodes and
  148.9 seconds. Mean utility regret was 0.017284 at depth 3, 0.008338 at
  depth 4, 0.000007 at depth 5, and zero at depth 6. The depth-4-to-5 gain
  was 0.008332 per position, almost entirely one tie-to-win correction.
  Depth 6 removed only one remaining four-spread-point miss
  (`0.000007` utility per position).
- Median added work in the first hard corpus was 191K nodes for depth 4,
  684K for depth 5, and 1.18M for depth 6, but depth-5 EBF had a 29.2 p90
  and the extreme tails are operationally important. Rack size alone did not
  identify them: two of the three 60-second tails were 12-tile positions.
- Move/value settling is a promising online discriminator. Of the 58
  first-corpus positions with adjacent depth-3, depth-4, and depth-5
  observations, 41
  returned the same move and backed-up value at depths 3 and 4. All 41 had
  zero depth-4 regret and zero depth-5 gain. The 17 unsettled positions had
  mean next-depth gain `0.029406`, but its 95% interval
  `[-0.028254, 0.087066]` is dominated by the single tie-to-win event.
  This is a useful prior, not yet a calibrated guarantee.
- The independent hard replication confirmed the discriminator rather than
  that large effect size. Mean utility regret was `0.000007` at depth 4 and
  zero at depth 5. The two depth-4 misses were only three and one spread
  points, and depth 5 fixed both; there was no win-class correction. Depth 6
  added no quality in the 59 positions that reached it.
- In the replication, 47/47 positions whose move and backed-up value had
  settled from depth 3 to depth 4 had zero depth-4 regret and zero depth-5
  gain. Both misses were among the 11 unsettled positions. Pooling the
  adjacent-depth observations from both hard corpora, the rule would stop
  88/116 positions (75.9%) at depth 4 with no observed miss and buy depth 5
  for the other 28, which contained every observed depth-4-to-5 gain. The
  one-sided 95% binomial upper bound on a missed-regret *event* in the settled
  group is still about 3.35%; it does not bound severity.
- The replication also exposed the cost tail. One depth-6 solve finished at
  161.6M nodes/82.4 seconds. Another was still incomplete at 382.3M nodes and
  300 seconds; its depth-5-only reference took 72.5M nodes/85.8 seconds.
  Keep that depth-6 observation right-censored rather than treating depth 5
  as a depth-6 oracle.
- Fixed-depth-5 playthroughs over both independent hard corpora quantify
  future demand for the same player's clock. Across 120 independent initial
  positions, the sum of all later same-player endgame work was 0.3% of the
  current solve at p50, 3.0% at p90, 9.2% at p95, and 40.0% at empirical p99.
  The maximum ratio was 127.6%, so a ratio-only reserve is unsafe when the
  first solve happens to be cheap. In absolute nodes the corresponding
  quantiles were 3.6K, 51.8K, 170.3K, and 3.85M; the maximum was 13.50M.
  Twenty-eight of 120 starts needed no later same-player solve at all.
  This directly rejects equal slicing while also showing why future demand
  cannot simply be assumed to vanish.
- The provisional shadow reserve
  `csw24-eg-future-depth5-observed-max-shadow-v0-20260729` records the monotone
  envelope of pooled observed future-work maxima by combined rack tiles. It
  reserves at most 13,501,513 nodes at 13--14 tiles, 5,819,156 at 12,
  3,121,816 at 10--11, and declines rapidly below that. Runtime converts these
  portable nodes with the current solve's live seconds/node and an explicit
  lower-throughput multiplier (1.5 in the harness). This makes withdrawals
  possible without pretending later searches cost as much as the current one.
  Because both 60-position corpora supplied the maxima, this is a shadow-only
  observed envelope, not an independently certified p99. Leave-one-corpus-out
  coverage was only 96.5--97.2% across all turn states and 95.0--98.3% on
  initial endgame states; the worst miss was large. The pooled envelope is
  useful for scheduler traces, but a production p99 reserve needs more
  source-game clusters or a more conservative fallback.
- The current admission sample is still small enough that a distribution-free
  p99 bound is effectively the observed maximum. Pooling the two hard corpora,
  empirical EBF p99/max values for depths 4 and 5 were 42.6/118.6 and
  74.2/92.7. Added-node p50/p95/p99/max were
  169K/3.07M/6.24M/11.44M for depth 4 and
  516K/34.58M/67.79M/87.49M for depth 5. Depth 6 cannot yet claim a valid
  completed-only tail: one search had consumed about 310M additional nodes
  beyond its separately measured 72.5M-node depth-5 boundary and was still
  unfinished at 300 seconds. Right-censored attempts must enlarge admission
  risk rather than disappear from training. These loose tails make plain EBF
  a fallback envelope, not the production predictor. A conditional model must
  reduce slack while preserving held-out completion coverage.

The immediate scheduler prior is therefore more selective than a fixed target
depth. Easier endgames usually settle by depth 4 or 5. On a hard endgame,
offer depth 5 as a withdrawal when the depth-4 move or backed-up value is still
changing, especially when the backed-up value is near the win/tie boundary;
settling lowers its priority but is not an absolute stop. Depth 6 had low
observed marginal value (`0.000007` mean utility) and a severe cost tail, so
it should lose to valuable future work when the clock is scarce. With ample
time after reserving for the declining cost of later endgame turns, its shadow
price comparison can still be favorable and search should continue. Predict
the next whole-depth cost from recent node increments, EBF, and root work
rather than rack size alone. All zeroes and the settling separation are
finite-corpus observations and need conservative smoothing and full-game
policy validation before becoming hard rules.

Pooling the two independent 60-position hard corpora now has explicit corpus
identity in `analyze_endgame_value_curve.py`; overlapping position numbers no
longer overwrite one another. At the completed depth-4 boundary, 88/116
adjacent-history positions were settled (same move and backed-up value at
depths 3 and 4) and had zero observed depth-5 gain. The 28 unsettled
positions had mean depth-5 utility gain `0.017868`, 95% normal interval
`[-0.017139, 0.052874]`. Across all 120 positions the mean gain was
`0.004169`. This is dominated by one win-class correction and therefore is
not precise enough to ship as a value model.

The next value audit is frozen before looking at outcomes on the separate
300-position admission-validation corpus (seed `7295959`). The experimental
arm records choices through depth 5; both the depth-4 and depth-5 nominees are
then force-evaluated by a common depth-6 judge. The judge is therefore deeper
than either arm, matching the two-corpus training target. Only those two
nominees are scored (`MAGPIE_EG_CURVE_JUDGE_MIN_DEPTH=4`) to avoid wasting
fixed-depth re-searches on irrelevant shallow moves, while an unjudged
depth-3 context row preserves the settling feature. The oracle value is the
better common-judge value of the nominees, not automatically the move that
the unrestricted depth-5 root search returned. Each initial arm solve has a
60-second cap. Each distinct forced nominee also has its own 60-second judge
cap. `EGCURVETRUNC` and `EGCURVEJUDGETRUNC` preserve arm and failed-judge work
separately; neither contributes a regret row. Those source positions are tails
to rerun with a larger, explicitly affordable allocation, never zero-regret
examples. The provisional shadow-only target-5 prior is:

- settled depth 4: `1e-6` utility (an explicit exploration floor, not an
  empirical mean);
- unsettled depth 4: `0.017868`;
- missing adjacent history: global `0.004169`.

The exploration floor makes "ample clock" distinct from "proven no value":
when future shadow price truly approaches zero, a depth with a valid
completion bound can still be bought. Under scarce clock it should lose
immediately. The validation asks whether settling continues to contain all
material depth-5 gains and whether the pooled magnitudes calibrate; no
coefficient is retuned until that audit is reported.

That audit is now complete. Of 300 attempts, 294 had both a complete arm
through depth 5 and a complete common depth-6 judgment; four arms and two
judges hit their independent 60-second caps and remain censored. Completing
depth 5 reduced mean utility regret by `0.0000115646` (95% CI
`[0.0000040665, 0.0000190627]`) and improved exact-move agreement from 95.92%
to 99.66%. The provisional prior was grossly miscalibrated: it predicted mean
gain `0.005807`, about 500 times the held-out mean.

The held-out state means now replace that provisional magnitude:

- settled depth 4: `0.00000564972` (`n=177`; two positive-gain events);
- unsettled depth 4: `0.0000269663` (`n=89`; ten positive-gain events);
- missing adjacent history: observed mean zero (`n=28`), retaining only a
  `1e-6` exploration floor.

The unsettled group still has materially more value than the settled group,
so the qualitative feature survived; its earlier magnitude did not. The
runtime artifact is
`csw24-eg-target5-judge6-300-shadow-v1-20260729`. It remains shadow-only
because the production TT topology and completion-tail calibration are
separate requirements.

### Whole-depth admission protocol

The next-depth predictor is being evaluated separately from the value curve.
The development protocol was frozen before position 150 of the new corpus had
run:

- training: the two earlier 60-position hard corpora plus positions 0--149 of
  the 300-position corpus seeded at `7294949`;
- calibration: untouched positions 150--299 from that same corpus;
- final validation: 300 separately generated positions seeded at `7295959`;
- candidate selected on training data: ridge prediction of
  `log(added nodes / previous cumulative nodes)`, penalty 10, using search
  trace, root/ply-2 structure, rack split, and spread features;
- target: one-sided p99 whole-depth work, with every unresolved right-censored
  observation placed in the unknown upper tail.

These node distributions are hardware-independent but not solver-independent:
all current endgame observations use 10 ABDADA threads, the same TT fraction,
and the same heuristic settings. Thread count changes redundant work and
jitter, so production must condition on that configuration or collect a
separate calibration rather than applying the 10-thread coefficients blindly.
ABDADA also makes repeated attempts on the same position materially
stochastic. A tail rerun starts the depth again; it does not resume the
censored search. The analyzer therefore keeps attempts separate and uses the
largest observed exact work or censor lower bound per source position. A
cheaper completion may not erase a more expensive censored attempt.

No calibration or final-validation outcomes may be used to retune the model.
Report p95 as a diagnostic, but it is not a substitute for the preregistered
p99 gate. The final audit counts a false start whenever actual added work
exceeds the advertised bound, plus separately reports slack and false
rejections at representative available-node budgets.

Also audit a hardware-adaptive joint wall bound without changing the node
model:

```text
base wall prediction = predicted added nodes / preceding-depth live NPS
```

Conformalizing the final wall residual absorbs both tree-size error and the
depth-to-depth rate change in one bound, avoiding the incorrect multiplication
of two separately advertised p99 guarantees. On the frozen training set,
depth-5 NPS divided by preceding-depth NPS was 0.696 at p1, 0.826 at p5,
and 1.164 at p50 over 256 usable transitions. The node bound remains the
preregistered primary result; the joint wall result is a secondary diagnostic
until it receives its own untouched validation.

The frozen audit has now completed. The candidate is a plausible conservative
p99 gate, but the audit does not by itself certify a sub-1% production failure
rate:

- Calibration required residual multipliers of `20.421` at depth 4 and
  `27.599` at depth 5 for the node model. The corresponding jointly calibrated
  wall multipliers were `12.299` and `19.319`. These are the reproducible
  worst-attempt values after retaining, rather than erasing, expensive ABDADA
  reruns.
- The separate 300-position validation corpus had zero node-bound failures
  among 299 completed depth-5 searches, but one search was still unfinished
  after 1,800 seconds and 2.741B added nodes. Its lower bound already exceeded
  the frozen 1.739B-node prediction, so it is a definite failure, not missing
  data: 1/300 observed failures, with a one-sided 95% binomial upper bound of
  1.57%. One miss is fewer than the three expected in 300 at a true 1%
  failure rate, so this does not reject p99; it also cannot prove it.
- The depth-4 node gate had zero failures in 271 scorable positions, but even
  that only puts the one-sided 95% upper failure bound at 1.10%; it does not
  certify a sub-1% production rate.
- The secondary wall gate had one depth-4 failure in 271 positions. At depth
  5 it had one completed failure plus the definite censored failure
  (2/300, one-sided 95% upper bound 2.08%). When an adjacent-depth interval
  is unavailable, cumulative nodes/time through the preceding depth supplies
  a conservative live-NPS fallback, so all 300 positions are scorable.

The depth-5 miss is qualitatively important. After 24.4M cumulative depth-4
nodes, the ridge model expected another 63.0M and the p99 correction admitted
up to 1.739B, but the next depth had still not completed after 2.741B added
nodes. The conditional point model is useful, but alpha-beta's rare
branching/explosion tail is heavier than this sample/model captured. Do not
describe completed-only coverage as 100%, and do not confuse marginal p99
coverage with a guarantee that every admitted depth completes.

The miss is not an observed false start at the intended clock. At available
budgets through 1.5B nodes, the frozen gate had zero false starts on the
validation corpus; at 1.5B it admitted 298/300 positions and falsely rejected
only one completed search that would have fit. The extreme position's 1.739B
bound made the gate refuse it. The first observed false start appears at a
2.0B-node budget, where that position is admitted but its 2.741B lower bound
does not fit. A game-in-25-minute player at roughly 1M NPS cannot give a
single turn more than about 1.5B nodes even before future-clock reserve, so
the candidate remains operationally plausible for that target despite the
marginal-bound miss.

A post-hoc safety envelope of the form

```text
bound = max(conditional p99 bound, F * previous cumulative nodes)
```

is being explored, but it is not part of the frozen validation result. On the
already-seen validation corpus, `F=128` raises the unresolved case's bound
above its current lower limit, while increasing median bound/actual slack from
about 31x to 104x and admitting only 36 of 277 searches that actually fit
within a 10M-node budget. A repeat of the extreme position completed after
2.309B added nodes and 1,513.5 seconds, despite the earlier attempt already
having exceeded 2.741B nodes and 1,768.8 seconds without finishing. The first
attempt remains the conservative observation. Repeated-attempt calibration
and a new untouched corpus are required before selecting or claiming any such
floor. This is a safety/strength tradeoff, not a free calibration fix.

### Runtime whole-depth boundary

The solver now has an opt-in `EndgameDepthAdmissionCallback` at the same
globally useful completed-depth boundary used by the calibration trace. Its
request contains the current and two preceding published boundaries
(cumulative nodes, elapsed time, root/ply-2 structure, move, and value), plus
rack split, spread, scoreless turns, worker count, and remaining turn time.
Missing or skipped ABDADA history is explicit rather than imputed silently.

Shadow mode leaves every depth pre-admitted and observes the ordinary ABDADA
schedule. It never makes workers wait; this matters because even a nominally
"always admit" barrier at depth 3 changes the initial depth jitter, search
tree, and returned equal-value move. Enforced mode pre-admits depths through a
configurable warm-up (default 4). After that, the first worker to complete
depth `d` owns the decision for `d+1`, and no worker can enter `d+1` until the
decision admits it. A valid enforced decision needs a positive expected-node
estimate, a completion bound at least as large, confidence in `(0, 1]`, and a
matching calibrated solver/boundary configuration. Missing or malformed
predictions fail closed. Enforced admission replaces the old mean-EBF
soft-limit rule so two independent gates cannot race; the external hard
deadline remains emergency protection.

The exported artifact
`csw24-eg-ridge-p99-10t-v1-20260729` contains the exact 20-feature ridge
standardization and coefficients plus separate node and jointly calibrated
wall corrections. `tools/export_endgame_admission_model.sh` names and checks
every frozen raw input, refits the artifact, and reproduces
`src/impl/endgame_admission_model_default_data.h` byte for byte after
formatting. The large raw logs remain ignored experiment artifacts rather
than source files. Golden tests reproduce Python's rounded predictions for a
normal position and the 2.741B-node validation tail. Depth 4 is explicitly
shadow-only: the calibration workers were initially jittered through depth 4,
so blocking depth 4 at a completed-depth-3 boundary would change the protocol
whose work is being predicted. Depth 5 is enforceable after the default
depth-4 warm-up. Worker count, heuristics, and TT setup are also part of the
domain. In particular, the present PlayChooser external shared TT does not
match the private 5%-memory TT used for calibration, so its opt-in shadow
events are marked unsafe to enforce.

Analysis trace schema 5 adds `ADMISSION` events with expected work,
one-sided completion-bound work, confidence, admit/refuse, and the root
position fields needed to replay the prediction. Unit coverage establishes
both sides of the contract: a shadow refusal is result-neutral, while an
enforced refusal with multiple ABDADA workers returns the last completed
depth and emits a time-limit finish without any depth-`d+1` completion.
Schema 5 extends the admission prediction with explicit node, scenario, and
candidate coordinates, so PEG does not lose its hardware-independent work
model when the trace is serialized.

This is still not a production policy. PlayChooser has an opt-in shadow flag,
but its legacy allocation remains authoritative. Before enabling depth-5
enforcement, collect shadow traces with the actual production TT topology,
audit prediction slack and false-start classifications, and either recalibrate
for that topology or deliberately switch production to the calibrated private
TT setup. Depth-4 enforcement needs a new strict-boundary calibration corpus,
not reuse of the current shadow-only coefficients.

### Fresh PEG completion and value calibration (2026-07-30)

The earlier 80-position, stage-wide PEG pilot is superseded by a fresh
hardware-concurrency run on the M5. The admission corpus contains 320
independent, uncensored source positions per bag (1,280 total), plus 320
naturalistic deeper traces for each of bags 2 and 3. It measures the actual
minimum useful boundary at a new fidelity: an initial wave of exactly two
completed candidates. One candidate cannot re-rank anything.

The portable work observations are:

| boundary | bag | median scenarios / nodes | empirical p99 scenarios / nodes | empirical maximum scenarios / nodes |
|---|---:|---:|---:|---:|
| first two at 2 ply | 1 | 14 / 62,832 | 16 / 2,306,677 | 16 / 3,378,394 |
| first two at 2 ply | 2 | 58 / 155,348 | 144 / 2,790,781 | 144 / 5,110,942 |
| first two at 2 ply | 3 | 266 / 212,699 | 1,056 / 4,571,562 | 1,056 / 7,399,314 |
| first two at 2 ply | 4 | 1,208 / 186,176 | 10,944 / 11,326,478 | 15,840 / 29,684,225 |
| first two at 3 ply after 16 at 2 ply | 2 | 64 / 2,417,603 | 144 / 173,175,320 | 144 / 637,317,412 |
| first two at 3 ply after 16 at 2 ply | 3 | 317 / 1,205,910 | 1,056 / 130,966,268 | 1,440 / 452,287,912 |

All rows have 320 complete observations and zero final censoring. The prefit
flat componentwise envelope had zero false starts on 64 prospectively held-out
positions in five of six rows; bag 1 at 2 ply missed once. The rejected
feature-conditioned conformal model missed more rows and was often much more
conservative. The full-corpus maximum is therefore the better shadow envelope,
but it is not a certified deadline guarantee: under exchangeability it has
only `1 - 0.99^320 = 95.99%` probability of covering the population p99. A
production gate still needs an explicit safety factor and online false-start
telemetry.

The loaded-M5 local wall fit over 4,480 completed boundaries was

```text
seconds ~= 0.664960
        + 4.490317e-6 * scenarios
        + 5.162207e-7 * nested nodes
```

with `R^2 = 0.804`; its small negative candidate coefficient is clamped to
zero. The intercept is retained as a per-wave cost. This is only a local clock
conversion. The scenarios/nodes/candidates remain the strength-portable
coordinates, and a live machine/load model must replace these coefficients.

The separate quality corpus has 200 independent source games, 50 per bag.
Every one of the 73 policy-disagreement positions completed a common direct-4
judge; direct 4 changed the best direct-3 nominee on 9/73 positions (12.3%).
The completed 2-ply marginal observations were:

| added candidates | utility gain (95% CI) | changes / 200 | mean added scenarios / nodes |
|---|---:|---:|---:|
| 2→4 | +0.00780 [+0.00170, +0.01664] | 30 | 753 / 444,791 |
| 4→8 | +0.00492 [+0.00017, +0.01144] | 18 | 1,609 / 799,677 |
| 8→12 | -0.00357 [-0.00891, +0.00004] | 11 | 1,592 / 950,357 |
| 12→32 | +0.00410 [+0.00005, +0.01061] | 18 | 7,978 / 5,060,633 |

The non-monotone finite-panel curve is real: each checkpoint may return a
different move, and the common oracle rather than the later checkpoint defines
value. Raw means must not be forced into a monotone scheduler prior. The best
adaptive point estimate was minimum 8 / stability patience 4, which averaged
8.20 candidates and had full-32 regret `+0.00025` with 95% CI
`[-0.00683, +0.00772]`. It did not establish the requested noninferiority
margin. With genuine surplus clock, the rare 12→32 rescues retain positive
measured value.

Completed `{16,8}` 3 ply improved on the adaptive 2-ply policy by `+0.00282`
utility, 95% CI `[-0.00028, +0.00763]`, at a mean incremental 79.98M nested
nodes. `{24,12}` added `+0.00165`, 95% CI `[-0.00000, +0.00443]`, for another
31.70M nodes. The effect is not a simple bag-2/3 rule, while naturalistic
first-two 3-ply completion tails exist only for bags 2--3. The small older
4-ply experiment and the severe direct-4 cost tail both favor completed 2-ply
width and a completed 3-ply stage over starting 4 ply at roughly ten local
minutes.

`src/impl/peg_time_manager.[ch]` freezes these observations as shadow artifact
v1. It can plan exact first-two 2-ply waves for bags 1--4 and exact first-two
3-ply waves after a completed 16-candidate 2-ply boundary for bags 2--3. It
also exposes the direct-4 2-ply value observations without automatically using
them as priors. A normal 0.99 policy fails closed on the 95.99% tail evidence,
and every decision is still marked unsafe to enforce.

Ordinary production PEG remains stage-wide: it launches 8/16/32 candidates
behind one barrier. Those requests now deliberately fail the exact-wave
configuration match instead of borrowing a two-candidate envelope. The next
dispatcher change is to submit the first two as one wave, publish a usable
result, then submit one candidate at a time and replan only after completion.
Before enabling it, calibrate single-candidate post-wave tails, choose and
validate a safety factor, and add online miss telemetry. The direct-4 value
curve supports aiming for eight 2-ply candidates when work fits, but not a
hard strength-preserving stop at eight.

`src/impl/time_manager.c` is the policy boundary. It keeps portable work
coordinates separate, converts each proposed result-boundary chunk into local
seconds, and buys sequential chunks only while:

```text
expected regret reduction / predicted seconds > future shadow price
```

The plan reports its deposit (positive) or withdrawal (negative) relative to
equal slicing. It is not connected to live PlayChooser decisions yet; that
waits for held-out calibration strong enough to supply the chunk values.

The endgame admission model now has a one-boundary TimeManager bridge for
shadow evaluation. At every completed depth it places expected nodes and the
calibrated completion-bound nodes in one portable endgame chunk, preserves the
jointly calibrated expected/bound wall conversions, adds current safety and
future-turn reserves exactly once, and replans only that next depth. The raw
clock fit, calibrated topology, completion confidence, future shadow price,
and marginal-regret test must all pass. A valid prediction from a mismatched
topology remains visible in shadow mode but fails closed under enforcement.
The bridge can also take a future reserve in nodes; it converts that reserve
with the boundary's live seconds/node and a caller-visible lower-throughput
multiplier before adding it to any explicit seconds reserve. Thus the
trajectory artifact remains hardware-independent while a loaded or throttled
machine protects more wall time automatically.
The solver request now keeps two clocks separate: its current-call hard window
is the non-negotiable completion gate, while the player's total game clock is
the cross-turn allocation bank. TimeManager uses the latter; the raw admission
model still requires the next depth to fit the former. Conflating them would
divide an already-sliced move budget by the estimated turns again and would
make a genuine withdrawal impossible.
The bridge is not wired to live PlayChooser admission yet because the
position-aware regret-reduction callback and production shared-TT calibration
are still missing; neither should be replaced by an arbitrary constant in a
shipping policy.

## Validation

1. Fit curves on source-game-clustered training positions.
2. Check oracle regret on held-out positions at fixed work budgets.
3. Replay full games under an identical starting clock and overtime rule:
   equal slicing versus TimeManager.
4. Log every deposit, withdrawal, predicted gain, realized work, and returned
   result boundary.
5. Oracle only differing moves, then report game-level utility/spread/win
   deltas and time-penalty incidence.

The policy succeeds only if withdrawals convert earlier savings into lower
game-level regret. A policy that merely finishes with more unused clock has
failed its objective.
