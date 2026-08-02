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
expected current regret reduction
    > F(future state, future clock - predicted chunk seconds)
      - F(future state, future clock)
```

For a sufficiently small interruptible checkpoint, dividing both sides by its
seconds recovers the local `current value/second > lambda` rule.

and the safety reserve remains intact. Stopping before the old equal slice is a
deposit. Continuing past it is a withdrawal. The equal slice is a comparison
baseline, not a hard boundary.

Chunks align with useful result boundaries:

- sim: a node tranche ending at the next regret-estimator check;
- endgame: the predicted nodes needed to finish the next IDS depth;
- PEG: the predicted nodes needed to finish the current candidate, then the
  current stage. Do not buy half a candidate whose result will be discarded.

## Learned rest-of-game value

Current-search confidence is not a time-allocation policy. A BAI posterior can
estimate sampling regret inside the candidates and rollout policy it has seen;
a PEG or endgame trace can estimate the value of its next completed boundary.
Neither says how valuable the same second would be on positions that have not
yet occurred. TimeManager therefore needs two independently calibrated sides
of every decision:

```text
current gain = E[R_current(work now) - R_current(work after next chunk)]
future loss  = F(next state, future clock - chunk seconds)
             - F(next state, future clock)
```

The true target is game-level, not a sum invented for convenient telemetry:

```text
J(state, clock) = maximum expected terminal win/spread utility achievable
                  by the search-and-play policy over the rest of the game
F(state, clock) = J(state, unlimited clock) - J(state, clock)
```

Its finite difference is the price of a proposed chunk. For an indivisible
PEG wave or endgame depth use
`F(state, clock - chunk_seconds) - F(state, clock)` directly; a single local
slope `lambda` is only an approximation for a tiny simulation checkpoint.
Recompute after every completed boundary because the curve can steepen as the
clock shrinks. Buy the next chunk only when current gain exceeds that future
loss, subject to the separate hard completion and clock-safety gates. A fixed
residual-regret target is not a substitute for `F`: it spends the same way in
a trivial position with surplus clock and in a hard position before several
more valuable turns.

The first offline labeler uses the minimum expected sum of oracle decision
regrets over later turns as a *surrogate* for `F`. That decomposition is valid
in the ideal performance-difference setting only when every per-turn oracle is
a consistent action-value estimate for the same continuation policy and the
expectation uses the evaluated policy's state distribution. Our finite-horizon
judges and fixed observed trajectories do not fully meet those conditions.
Suffix-DP labels can therefore train and debug the first model, but cannot by
themselves pass the live gate. Held-out complete-game allocation backtests must
verify terminal win/spread utility, including changed future positions.

Training labels come from full-game, source-game-grouped work/value curves.
For every observed turn, retain discrete legal result boundaries:

```text
(mode, work, oracle regret, result identity)
```

Then solve an offline suffix allocation problem for many remaining-clock
budgets. For a realized sequence of same-player turns `t..T`:

```text
F_t(B) = min over legal work choices n_t..n_T
         sum_i R_i(n_i), subject to sum_i seconds_i(n_i) <= B
```

This dynamic program creates initial supervision for both total rest-of-game
regret and the marginal value of the next unit of clock. It must include a terminal
state where unused time has zero value and the configured overtime penalty,
otherwise a model can learn to hoard clock forever. Training, calibration, and
test splits are by complete source game, never by position, so future turns of
the same game cannot leak into the fitted value-to-go.

The runtime artifact should remain hardware-independent. Source curves store
mode-native opportunity curves (simulation rollout nodes, endgame nodes at
completed depths, and PEG scenarios/candidates/nested-endgame nodes). Runtime
rate profiles convert those opportunities to seconds during suffix-label
generation; the model is conditioned on that rate-profile grid. Live robust
work rates select/interpolate a profile. Given the remaining clock and
predicted future phase mix, a small monotone solver evaluates `F` and its exact
finite difference. This lets the same portable artifact react to throttling or
contention rather than learning the calibration machine as a hidden feature.

The first auditable predictor is a hierarchical monotone table:

```text
rate profile -> bag phase -> expected future own turns -> score-spread band
             -> combined rack-size band
```

Sparse cells shrink toward their parent and every clock curve is forced
nonincreasing in expected regret. This is intentionally less expressive than
a generic tree ensemble: it is fast enough for every SIM checkpoint and makes
bad extrapolation visible. More state features earn their way in only through
source-game-held-out policy regret, not in-sample fit.

Useful state features must be known before the future positions occur:

- bag count, both rack sizes, score spread/win region, and expected own turns;
- predicted counts of future SIM, PEG, and endgame decisions;
- current clock, overtime schedule, and live mode-specific work rates; and
- only for the current opportunity, search-progress features such as candidate
  count, arm gaps/variances, completed PEG stage, or completed endgame depth.

Current-search features must not be smuggled into forecasts for unknown future
turns. Position-specific future difficulty is integrated out by the learned
conditional distribution. The first model can be a conservative monotone
table/spline by phase and clock pressure; a more expressive model is justified
only if held-out source games show better regret calibration.

The existing experiments do not yet identify this full model. They provide
three important ingredients: average SIM regret-versus-node curves, one
operating-point calibration of conditional BAI sampling regret, and discrete
PEG/endgame cost/value boundaries. They do **not** provide a matched
production-topology curve at every turn of complete games, nor an online
forecast of the unseen suffix. In particular, the current thinking-curve
corpus ends around bag 50, uses top-60 candidates, and most regret panels use
6-ply at 300K nodes, while production PlayChooser uses top-15 and defaults to
2-ply. Those data may initialize priors, but they cannot certify a live
rest-of-game allocator.
Concretely, the reusable July 29 panel contains 168 SIM turn curves from 21
source games, all at bag 50--86. The 72-pair live TimeManager run recorded
3,388 turns but predates replayable per-turn CGP and seed fields, so it cannot
be converted into the missing full-game opportunity corpus after the fact.

Before allocation changes are re-enabled, collect matched top-15 curves along
complete game trajectories, join the held-out PEG/endgame boundary data, fit
the suffix value model, and back-test these separately:

1. conditional current-search regret;
2. oracle current-turn regret, including candidate/depth/horizon bias;
3. predicted value of the next work tranche; and
4. predicted rest-of-game regret and shadow price.

The game-pair trace should log all four. A reverse sum of estimates observed on
the eventual path is useful retrospective accounting, but it is not (4) and
must never be labeled as a pre-turn rest-of-game prediction.

The concrete shadow pipeline is:

1. `tools/collect_time_value_positions.py` records complete sequential source
   games with every pre-move CGP and rejects killed/truncated games. Collect a
   declared mixture of static trajectories and trajectories that actually
   play short-PlayChooser moves; the former are cheap and reproducible while
   the latter cover policy-induced states. The trajectory policy is retained
   as provenance and never inferred from a post-game outcome.
2. `thinkingcurve` accepts raw positions, generates the production top-15
   panel, and measures independent fixed-node SIM arms with a common deeper
   judge. Bag filters keep SIM at bag > 4 while preserving source-position IDs
   for later PEG/endgame joins.
3. `tools/convert_time_value_work_costs.py` applies a grid of runtime-rate
   scenarios to native work coordinates.
4. `tools/build_rest_game_value_labels.py` solves each same-player realized
   suffix independently for every rate profile and clock budget.
5. `tools/fit_rest_game_value_model.py` fits the monotone hierarchical table.
   It always emits a shadow artifact: predictive error alone cannot enable it.
6. `tools/backtest_rest_game_value_policy.py` replays the learned allocator and
   equal slicing turn by turn on held-out complete source games. It clusters
   uncertainty by source game and may pass only the realized-trajectory
   oracle-regret *surrogate* gate.
7. Mirrored game pairs must then improve terminal utility before the compiled
   artifact's live gate is set. A source game is keyed by seed and starting
   seat, not a corpus-local game number, and predicted future-turn features
   are computed from the current bag/rack state rather than realized suffix
   length.

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

`src/impl/peg_time_manager.[ch]` freezes these observations as artifact v1.
Its default API remains fail-closed: a normal 0.99 policy rejects the 95.99%
tail evidence, and enforcement additionally requires an explicit provisional
opt-in. It can plan exact first-two 2-ply waves for bags 1--4 and exact
first-two 3-ply waves after a completed 16-candidate 2-ply boundary for bags
2--3. It also exposes the direct-4 2-ply value observations.

The first experimental PlayChooser policy opted into that finite-corpus evidence
without mislabeling it as certified p99. It uses a 0.95 evidence threshold,
multiplies the empirical maximum's portable nodes and scenarios by 1.5, and
uses a separate 1.5 slowdown in the deadline wall conversion. The 1.5 factors
are conservative engineering choices, not values selected on an untouched
safety-factor sweep. They require an online audit before a release default.

Candidate-level enforcement changes only the no-poll 2-ply dispatcher. It submits the first
two candidates together, then submits one candidate at a time with the whole
worker pool and replans after every completed candidate. The intended
stability rule cannot stop before eight candidates and then stops after four
consecutive candidate completions without a change in the leading move, with a
hard cap of 32. Admission was nevertheless independent and could refuse a
candidate before eight; the long-match audit below showed that this distinction
was unsafe. A deliberate stop publishes the completed prefix as a usable
partial 2-ply tier; a deadline-interrupted candidate is discarded. Shadow and
interactive-poll solves preserve their old topology.

The first-two price is the calibration median. A later single-candidate price
uses half that median, but its admission bound deliberately retains the
*whole* first-two empirical maximum before applying the 1.5 multiplier. This
is a conservative proxy, not a separately measured post-wave tail. The value
prior spreads the measured 2→4 and 4→8 direct-4 gains over their candidates;
beyond eight it uses the much smaller full-32-minus-fixed-8 rare-rescue mean.
The unmeasured greedy→2 gain borrows the 2→4 block only as a weak prior.

Wall conversion starts from the loaded-M5 fit, adjusted inversely for the
current worker count. Once the same solve has completed its greedy prefix, the
policy rescales the observed work coordinates in both expected and deadline
models by actual/modelled prefix time, clamped to 0.25×--4×. Unobserved
coordinates retain their cold-start rates. This is a load-adaptive bridge, not
a replacement for a larger cross-hardware calibration. Benchmark telemetry
reports admitted chunks and deadline false starts; user interrupts are
excluded from the miss count.

Pair 45 of the first long match demonstrated why that qualification matters.
Its bag-3 prefix completed 49,276 scenarios in 0.691 seconds but searched zero
endgame nodes. The old uniform 0.43× live scale was nevertheless applied to
the endgame-node coefficient and admitted candidate three; that candidate then
consumed the remaining 15-second window without completing. The corrected
planner scales scenario/fixed overhead from this prefix but leaves the
unobserved endgame-node rate conservative, so the same recorded request is
refused before launch.

The candidate experiment buys only a 2-ply tier. It does not attempt the
calibrated 3-ply boundary because the complete path to 16 shallow candidates,
the post-wave deep-candidate tail, and a sufficiently precise deep value prior
have not all been validated together.

`src/impl/time_manager.c` remains the policy boundary. It keeps portable work
coordinates separate, converts each proposed result-boundary chunk into local
seconds, and buys sequential chunks only while:

```text
expected regret reduction / predicted seconds > future shadow price
```

The plan reports its deposit (positive) or withdrawal (negative) relative to
equal slicing. The original timed-autoplay integration treated the player's
real remaining clock as the bank: a clock/latency reserve and the observed-max
future depth-5 endgame trajectory were protected, and the rest became the
current physical window. The future reserve stays in portable nodes until
runtime, when the live PEG nested-endgame rate and a 1.5 lower-throughput
factor convert it to seconds. The audit below moved both PEG directions behind
a shadow gate; overtime and untimed/fixed-time paths retain legacy behavior.

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

### Cross-phase spend-down bridge (2026-07-31)

The first common-RNG game-pair match exposed a different integration failure:
the calibrated PEG policy saved `108.7` seconds over 20 games, but only `9.6`
seconds reappeared as additional simulation time. TimeManager consequently
finished with about `5.5` more unused seconds per game. Its 9--11 game result
and `-17.4` spread/game estimate were inconclusive, but the clock accounting
was decisive: a deposit with no later withdrawal has no game value.

Before PEG, the timed PlayChooser now forecasts the protected late-phase work
instead of assigning it equal future slices. The forecast consists of:

- the worst expected bag-1--4 time to complete PEG's minimum useful
  eight-candidate 2-ply prefix; the measured first pair pays one boundary
  setup cost and each later single candidate pays a fresh setup cost plus half
  the pair's portable variable work;
- the existing depth-5 future endgame trajectory in portable nodes, converted
  at runtime with the local PEG nested-endgame rate and the existing 1.5
  lower-throughput factor; and
- the ordinary response/overtime safety reserve.

The remaining discretionary clock is divided only across the estimated sim
turns before PEG. This releases predicted PEG savings early enough for sims to
use them. The bridge is deliberately one-sided: its budget is
`max(legacy_equal_slice, spend_down_slice)`. Missing calibration, an invalid
forecast, or a sufficiently slow worker topology therefore preserves the old
allocation exactly. It cannot make a sim turn shorter, and it does not weaken
PEG candidate or endgame-depth completion gates. At the opening the protected
late work is typically close to the equal slices it replaces, so the policy
does not manufacture a large speculative withdrawal; it releases time as the
real clock moves ahead of the remaining-work forecast.

The original integration applied the same floor inside the low-bag PEG budget.
Pair 68 exposed a path
that subtracted the PEG/endgame reserves and returned zero with 20.267 seconds
still on the player's clock, although legacy equal slicing offered 6.737
seconds. When the protected forecast does not fit, that path now returns the
legacy slice rather than forcing a static fallback; the later audit also
restored the ordinary depth cascade and shadowed all PEG budget differences.

The remaining-sim-turn divisor keeps the ordinary eight-tiles-per-pair mean
but adds one contingency turn. In a 12-pair/24-game trace panel, the raw mean
estimate undercounted 40 of 463 pre-PEG decisions, always by exactly one turn;
adding one covered all 463. This matters most near the boundary: an initial
validation run spent down at bag 12, the next two plays removed only seven
tiles, and an unforecast bag-5 sim turn then consumed the intended PEG reserve.
That run was stopped and excluded. The contingency preserves the observed
coverage without replacing the useful mean by an overly pessimistic
five-tiles-per-pair rate across the entire game.

The corrected validation then exposed the analogous boundary inside PEG. In
3 of 42 observed player/game trajectories that reached PEG, the player had two
PEG turns; all three were bag 4 followed by bag 1. An indivisible bag-4 first
stage once consumed its entire 28.7-second physical window and left the bag-1
turn with no clock above the protected endgame reserve. Reserving a second full
eight-candidate proxy proved too pessimistic: two validation pairs then made no
cross-phase withdrawal because that model serializes a fresh fixed boundary
cost for every later candidate, while observed second-turn prefixes completed
in roughly 0.7--0.9 seconds. The refined pre-sim forecast protects one full
minimum eight-candidate prefix plus the worst expected bag-1--3 first-two entry
wave. A live bag-4 PEG budget separately holds back that entry wave; its live
measurement then admits toward eight. Other PEG entries keep the single-prefix
policy. The validation gate counts any TimeManager static fallback at bag 0--4
as a failure.

Before a long strength match, require a five-pair operational panel with
mirrored RNG streams and full turn/candidate/depth traces. Admission requires
zero penalties, zero trace drops, no low-bag TimeManager fallbacks for lack of
budget, no PEG candidate false starts, and clear accounting evidence that
reduced PEG time is reallocated to earlier sims rather than merely increasing
the terminal clock. Match strength remains a separate, much larger experiment.

`tools/run_time_manager_match.py` preserves that protocol for both the gate and
the subsequent long match. It writes every turn, sim-arm sample count, PEG
candidate completion, and endgame call to joinable CSVs; records terminal
clocks and per-mode time in the pair summary; and treats the two-game pair as
the inference unit. A time-based run persists its original wall-clock deadline
before launching the first pair, so restarting cannot silently extend a
24-hour experiment. Resume also fails closed if its base seed, binary hash,
runner hash, thread count, clock, or other recorded protocol settings differ.

The final five-pair gate passed on the 10-core M4 Mini. All 10 games and their
candidate/depth joins audited with zero event drops, penalties, low-bag static
fallbacks, or PEG false starts. The spend-down policy released `4.803` seconds
of direct legacy-counterfactual sim budget over 13 turns. It retained `31.971`
seconds per player/game at the terminal position versus `25.960` for equal
slicing, so this conservative bridge still leaves room for a later online
hardware/position calibration. The five-pair strength point estimates favored
equal slicing (`-39.4` spread/game, `-0.200` win score, and `-0.1627` terminal
utility for TimeManager), but none was significant (`p = 0.091`, `0.178`, and
`0.158`, respectively). This panel is an operational admission test, not
strength evidence; the common-prefix median was only 11 turns.

### First long-match audit and constrained PEG fallback (2026-08-01)

The first long match was stopped after 72 complete game pairs when the trace
found one candidate false start (pair 45) and one zero-budget low-bag fallback
(pair 68). Its aggregate result was still inconclusive: TimeManager's paired
spread was `-3.26` points/game with 95% CI `[-11.88, +5.35]` (`p = 0.453`),
and neither win score nor terminal utility was significant. More importantly,
the common-RNG trace separated allocation effects from ordinary wall-timed
search noise:

- 12 pairs played identical moves throughout;
- 49 of the 60 first divergences occurred in SIM (47) or endgame (2) with
  exactly the same nominal move budget, so they do not test a TimeManager
  allocation decision; multithreaded, deadline-limited search can complete a
  different number of iterations despite identical RNG streams; and
- only 11 first divergences coincided with an allocation change: ten PEG roots
  and one SIM root. Their descriptive mean spread was `-10.77`, with 95% CI
  `[-23.84, +2.29]`. That small, selected subset is concerning enough to
  oracle, but terminal play after the first split is not causal evidence about
  the root move.

Clock accounting showed a real policy weakness independent of game outcomes.
Across the 72 pairs, TimeManager used `511.112` seconds in PEG versus
`1369.584` for equal slicing, but only `45.101` seconds was explicitly released
into earlier SIM turns. It finished with `28.280` seconds per player/game
versus `20.716`: useful reserve, but too much of the PEG deposit still reached
the terminal position unspent. Further strength tuning should therefore use
oracle regret at the ten differing PEG roots before either relaxing or
tightening admissions globally.

Pair 68 showed that a future-reserve shortfall must never turn usable clock
into a zero-budget static move. The first repair gave the legacy equal slice
only to PEG's greedy ranking. The broader oracle audit below rejected that
restriction too: shallow PEG checkpoints are not reliable substitutes for a
completed deeper stage. The production fallback now runs the ordinary PEG
cascade under the legacy equal slice. The turn trace records
`reserve_shortfall` and the match audit requires the turn to complete PEG
without a static fallback. A bag-4 shadow plan still prices the calibrated
entry wave for a possible bag-1 follow-up, but it cannot yet shorten live PEG.
Fixed-per-move and untimed analyses do not carry this cross-turn reserve.

The first oracle follow-up exposed a second, distinct PEG hazard. At pair 4's
bag-3 root, TimeManager spent `23.900` seconds versus an equal slice of
`14.240` and chose `WEM`; equal slicing chose `pass`. Common-sample direct
2/3/4-ply judges all preferred `pass` (by about 15, 25, and 24
spread-equivalent points respectively). A controlled 6/12/24-second PEG sweep
returned three different moves; the 6- and 12-second runs never finished root
nomination, while the 24-second run finished root but only one of 32 refinement
candidates. Thus extra time was changing an interrupted shallow incumbent,
not purchasing a calibrated quality boundary.

The ten allocation-linked PEG roots then received common-sample direct-3
judges. The old live policy won only one root and lost nine; its selected-root
mean utility delta was `-0.07089`. This is not a population strength estimate,
but it exposed a categorical implementation error. The advertised minimum of
eight 2-ply candidates constrained only the stability stop; candidate
admission could still refuse candidates 3--8. At pair 15 the live policy used
only `1.922` of its `19.250` seconds, stopped after six 2-ply candidates, and
played `EN`. The direct-3 judge preferred equal slicing's `GRIM` by `0.564875`
utility. Fixed 8, 12, and 16-candidate 2-ply replays all still chose `EN`; the
ordinary cascade completed 32 at 2 ply and 16 at 3 ply and recovered `GRIM`.
The problem is therefore depth allocation, not merely an off-by-two minimum.

Replaying all ten roots with the ordinary PEG cascade and the proposed outer
caps exactly recovered the equal-slice move in five cases. It also found two
oracle improvements (`AWN` over `pass` at pair 4 and `MEL` over `dON` at pair
47), one small loss (`HIRE` versus `HIE` at pair 46), and retained the two
larger losses at pairs 33 and 43. The selected-root mean delta improved to
`-0.01369`, but remained negative and was dominated by pair 43's incomplete
bag-4 root nomination. That is not sufficient evidence to enforce deposits.

The live gate is consequently symmetric and fail-safe: calibrated PEG
deposits and withdrawals are both shadow-only, actual PEG receives exactly the
legacy equal slice, and candidate-level admission is disabled in PlayChooser.
`peg_deposit_capped` and `peg_withdrawal_capped` record the shadow direction;
`reserve_shortfall` identifies a shadow recommendation below the minimum move
budget, and `peg_shadow_budget_ms` preserves the rejected recommendation's
magnitude whenever it is executable (otherwise zero). The generic candidate
dispatcher and its calibration remain
available for experiments. Unused time from an early-completing ordinary
cascade is still banked naturally by the game clock, while earlier SIM
spend-down remains enabled. Re-enforcement requires a depth-aware policy that
predicts complete stages and passes held-out oracle replay.

Finally, every traced turn now includes a player-on-turn-normalized CGP. The
two mirrored roots must be identical through the first divergence. This makes
the exact candidate disagreement directly replayable and removes seed-plus-
history reconstruction from future oracle analysis.

### Complete-stage PEG admission (2026-08-01)

The replacement policy plans at PEG's actual publication boundary: every
survivor at the next depth. It makes one decision before the stage-wide
scenario barrier. A refusal launches no work and leaves the previous complete
depth published. If an enforced admitted stage nevertheless reaches its hard
deadline before every candidate finishes, PEG counts one false start,
discards every result and per-scenario outcome from that depth, and restores
the previous complete ranking. Candidate-level admission is disabled whenever
this mode is selected, so a collection of individually admitted candidates
cannot masquerade as a completed depth.

The portable predictor uses information that exists at the boundary:

- the exact sum of scenario counts for all selected survivors;
- for 3 ply and later, the exact node count those same survivors used at the
  preceding depth; and
- the immediately preceding whole-stage scenarios, nodes, and elapsed time to
  rescale the hardware conversion under current thermal/load conditions.

The first 2-ply depth has no exact-endgame observation from the current solve.
PlayChooser therefore retains a normalized seconds-per-endgame-node rate from
completed PEG work on prior turns. Its first PEG call remains legacy-only to
warm that rate. A larger bag-conditioned residual protects the cold case; a
smaller residual remains even after the rate is warm because the wall cost of
a counted endgame node is position dependent. A node-free greedy prefix may
make expected pricing cheaper, but it may not shrink an unseen node tail. A
slow prefix still expands the deadline envelope. Later depths use the maximum
of a scenarios-based floor and a conservative multiplier on the selected
survivors' preceding nodes. Unsupported depth/topology combinations fail
closed.

An initial implementation incorrectly extrapolated a two-candidate empirical
maximum across 32 candidates and was operationally useless. The direct
whole-stage v1 replacement looked safe under an audit that counted any stage
finishing inside the generous 180-second observation window. That audit was
too weak: production would start as soon as the model's much smaller bound fit
the clock. Under the corrected definition, an admitted stage is a false start
if it is incomplete *or completes after its own predicted bound*. Replaying 47
prospective roots found 40 v1 admissions and nine false starts.

Each failed panel was then reclassified as training before a new artifact was
frozen. V2 widened the node envelope but missed a warm bag-1 root: 44.43s
actual versus a 31.99s bound even though its 7.80M nodes were below the 16.5M
node cap. V3 added a persistent hardware/load rate with 1.75x deadline safety,
but a fresh balanced block still found a bag-2 stage at 99.47s versus 43.34s
and a bag-3 stage that completed only 25/32 candidates in 178.71s versus an
87.63s bound. A single normalized NPS rate was not enough.

V4 retains the portable rate and adds outward-rounded warm residuals by bag
(2.0x, 3.0x, 2.5x, 1.25x for bags 1--4). Deeper stages use the immediately
preceding depth and retain a 1.0x residual. It replays every v1--v3 failure
panel with zero strict false starts. Its first untouched four-root smoke block
also had zero strict misses: 2/3 warm 2-ply stages, 3/3 warm 3-ply stages, and
2/3 4-ply stages were admitted and completed within their bounds. The bag-4
root warmed the runtime rate and is excluded from the gate. The subsequent
expansion stayed safe but was stopped after 14 roots because the full-32 rule
was operationally useless: it admitted only 3/14 warm 2-ply, 5/13 warm 3-ply,
and 3/12 warm 4-ply opportunities.

V5 kept the conservative envelope and, before launching any work, tried the
ordinary complete survivor prefixes 32, 16, and 8. The selected depth remains
indivisible: a deadline-truncated admitted depth is discarded in full and the
exact previous-depth ranking is restored. Its first direct prefix-8 block
found no envelope miss, but exposed a bookkeeping defect: if the deepest
scored stage used zero exact-endgame nodes, it erased a valid shallower
positive-node hardware-rate sample. That panel is training evidence, not
held-out evidence.

V6 fixes both production and replay to retain the deepest positive-node stage.
It also refuses invalid candidate pointers and restricts adaptive narrowing to
the frozen 16/8 cutoffs rather than inventing an uncalibrated midpoint. Three
untouched balanced blocks directly exercised every possible adaptive
trajectory: 8/8/8, 16/16/8, and 32/16/8. Across the combined 12 roots, the one
cold hardware-rate root was excluded as preregistered. The strict replay then
had 8/11 warm 2-ply admissions, 4/11 warm 3-ply admissions, and 2/10 warm
4-ply admissions, with zero false starts. Every incomplete observed depth was
prospectively refused, including bag 2 at 1/8 candidates in the prefix-8 block,
bag 4 before completing its second prefix-16 depth, and bag 3 at 5/8 in the
full-prefix block. This is encouraging safety evidence, but still far short of
the usefulness and sample-size gates. The frozen artifact SHA-256 is
`35a445a9bfefc11674bc8a100f2fef69099700bcd2fa41ecf82103c9376426c4`;
the validating binary SHA-256 is
`8c0dee86beffdec70c38deeb71582ff7b517b7b0accfb3a4d067d2d4abf812d6`;
and the combined strict-analysis SHA-256 is
`06ff8d2f9c325220a8dc193e10d301bcdec4b530a37423884b48f8b21682b07a`.

The preregistered live gate requires at least 59 independently admitted stages
with zero false starts (`1 - 0.95^59 = 95.15%` evidence for p95) and at least
16 held-out roots in every bag stratum. Only admitted stages count toward the
evidence; refusing everything cannot pass. `heldout_gate_passed` remains false
in every compiled calibration and PlayChooser's live allocation switch remains
off. V1--v5 are retained to reproduce the failed analyses. The active frozen
artifact, resumable sequential runner, and strict replay audit are:

- `tools/peg_time_calibration/adaptive_complete_stage_admission_v6_20260801.json`;
- `tools/run_peg_stage_admission_validation.py`; and
- `tools/analyze_peg_stage_admission.py`.

### Game-pair regret back-test trace (2026-08-01)

The paired-match harness now records residual decision regret rather than only
terminal strength and work. After every simulation, PlayChooser copies BAI's
final expected regret in the exact blended-utility units used to rank its arms.
A genuinely forced one-candidate decision is zero. Static, PEG, and endgame
turns remain explicitly unknown: their marginal-value observations do not yet
constitute calibrated residual-regret models, so filling them with zero would
make the game forecast look much more certain than it is.

Each `PCTURN` row contains `regret_valid`, `expected_utility_regret`, and the
model name. Each `PCGAME` row independently totals known regret and
known/unknown turn counts for both players. The match auditor verifies those
totals against the turn rows and writes a reverse cumulative sum for every turn
on the *realized* later path. That rest-of-game column is retrospective
accounting over estimates that were individually made before their moves; it is
not presented as an online prediction of positions that had not yet occurred.
The human-readable match update also prints the latest pair's four separate
game/player totals and coverage, rather than hiding them in a policy average.

The pair summary reports two complementary back-tests:

- whole-game and post-first-divergence expected regret per policy, always with
  coverage counts; and
- before and including the first different move, `equal regret - TimeManager
  regret` on the identical mirrored roots. This is the model-predicted utility
  benefit of the allocation change. Later roots are deliberately excluded from
  that paired prediction because the game states are no longer comparable.

It also reports terminal utility minus the same-root prediction. Across many
independent game pairs, a calibrated estimator should have a mean residual near
zero and the predicted deltas should explain some of the observed utility
deltas. This is a calibration test, not a license to interpret uncovered
late-phase decisions as regret-free.

A one-pair 10-second-clock smoke test exercised the complete schema: all 44
turn rows reconciled with the two game totals, 34 turns had BAI estimates, ten
PEG/endgame turns remained unknown, 17 identical roots were comparable, and
the pair audit passed with no trace drops.

### Stratified rest-of-game value panel (2026-08-02)

The complete-game panel now has strict, mode-native curves for 133/140 source
games (68 PlayChooser trajectories and 65 static trajectories). A sparse
180-second continuation resolved 52/60 originally hard roots. All 366
endgames are usable; eight PEG roots across seven games remain censored. The
joined complete-game corpus has 3,006 turn roots and 31,530 measured work
boundaries. Source-game splitting leaves 93 training, 20 calibration, and 20
untouched test games.

The first shadow allocator favors learned allocation over equal slicing, but
does not pass its held-out surrogate gate. On the untouched games its mean
learned-minus-equal utility regret is `-0.001305`, with 95% CI
`[-0.003066,+0.000456]` and `p=0.146`. Calibration is `-0.003651`, with 95%
CI `[-0.007869,+0.000567]` and `p=0.090`. These are directional results, not
live-policy evidence; the terminal game gate also remains absent.

More importantly, the broad SIM panel stops at 300K nodes. Every one of its
2,493 SIM roots has at least one zero-oracle-regret hit among the 12
independently sampled depth/budget arms. Consequently the per-root lower
envelope saturates around 35 seconds of whole-game clock and an imputed tail
cannot improve it. This exposes oracle selection in the replay: a runtime
allocator cannot know which independently sampled arm happened to match the
judge. The replay is therefore an optimistic allocation upper bound, not a
valid three-minute stopping-policy test.

There is a second optimism bug in that panel: the online judge received only
the distinct moves nominated by the tested budgets. If every budget returned
the same wrong move, the single-candidate judge was forced to assign it zero
regret. The corrected harness can add a *checkpoint-observable risk set* for
each arm: its selected move plus the highest uncertainty-bound challengers,
chosen from that arm's means, variances, and sample counts. The union is sent
to the common-seed judge only after every nomination finishes. Oracle values
therefore score choices but cannot affect nomination, risk-set construction,
or stopping. A one-root top-15 smoke exposed the practical difference: the
4-ply policy returned the same move from 300K through 10M nodes, while the
eight-move risk-set judge found a different move worth `0.02335` utility. The
old nominee-only protocol would have reported zero at all four budgets.

An independent top-60 deep panel was retained only as a sensitivity prior.
After clustering by its 19--21 source games, lower-95% evidence credits a
6-ply regret retention of 0.846 at 1M nodes, 0.811 at 3M, and 0.629 at 10M
relative to 300K. It credits no p2/p4 tail improvement. Central and lower-95%
tail scenarios were both generated, but zero tail options survive the broad
panel's oracle envelope and their backtests are therefore exactly unchanged.
The top-60/top-15 topology mismatch is an additional reason these priors are
not live-eligible.

The replay itself now represents planning and scoring separately. A curve may
carry cross-fitted `expected_regret` for allocation and held-out `regret` for
scoring. Learned allocation can only see the former. Equal slicing commits to
the deepest completed boundary fitting its slice rather than selecting the
oracle-best affordable result after the fact. A legacy curve without the
separate expected field remains readable for diagnosis, but its replay is
explicitly counted as oracle-choice contamination and cannot pass the honest
surrogate gate.

Replaying the 133 complete games with cross-fitted planning regret for every
SIM, PEG, and endgame boundary removes the earlier directional result. On the
20 untouched games, learned-minus-equal scored regret is `+0.000763`, with 95%
CI `[-0.007069,+0.008594]` and `p=0.849`; calibration is `-0.001729`, with
95% CI `[-0.006864,+0.003407]` and `p=0.509`. All 2,986 accepted replays used
separate planning estimates and none used oracle regret to choose a boundary.
The estimator was conservative on selected actions: test actual-minus-
expected regret averaged `-0.004511` for learned allocation and `-0.006160`
for equal slicing. This is a valid null result for the choice protocol, but
not a final strength result: the broad SIM score still comes from the old
nominee-only judge and can miss a better move outside the nominees. The
matched risk-set tail panel supplies the next correction.

The next honest gate must choose current-turn work using only information
available at that checkpoint: either a cross-fitted expected-regret model or a
true cumulative solver trace carrying its contemporaneous regret estimate.
Held-out oracle regret may score that choice, but may not choose it. A matched
top-15 deep subset is also required to calibrate the 300K--10M tail relevant
to game-in-three-minutes clocks. The frozen follow-up panel contains 80 roots
from 80 distinct games, balanced 40/40 by trajectory policy and 10 per policy
in each of four bag phases. It measures independent 300K/1M/3M/10M arms at
4 and 6 ply and judges the union of eight-play checkpoint risk sets. Live
rest-of-game allocation remains off.

`tools/fit_sim_checkpoint_regret.py` implements the first of those gates. It
splits by complete source game and emits two distinct cross-fitted estimates.
The state estimate uses only pre-search state, candidate count, ply, and the
fixed node budget; its node curve is forced nonincreasing and can supply
future-turn opportunity curves. The checkpoint estimate adds only the BAI
regret estimate and best/challenger gap observed at that checkpoint; it is the
candidate current-turn stopping signal. The common judge supplies utility,
win, and spread labels only after every arm has finished. It is never a model
feature or an allocator input. The fitted artifact is shadow-only, and fixed
budget plus paired-tail summaries remain clustered by source game.

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
