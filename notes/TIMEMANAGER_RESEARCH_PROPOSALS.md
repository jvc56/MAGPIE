# TimeManager research proposals (2026-08-03)

Context: five consecutive iterations improved experimental validity without
improving the conditional checkpoint-regret predictor. The 2026-08-03
calibration failure (slope 0.782, 2/10 bins over 2x, worst ratio 43.97,
q95 mean upper 0.006498 vs actual 0.000366) motivates changing what is
predicted rather than predicting the same target harder. These proposals
preserve nonanticipation, complete-game inference units, optional-stopping
validity, hardware-portable work coordinates, fail-closed behavior, and the
separately preregistered mirrored terminal-game gate.

## Central diagnosis: the target contains a component no chunk can buy

Three existing observations point the same way:

1. **Regret plateau.** In the matched risk-set tail, p4 regret is ~0.000661
   at 10M nodes versus 0.000744 at 300K; p6 is 0.000057 at both 3M and 10M
   with 0/80 choice changes. Additional same-configuration nodes almost never
   change the answer. The residual is disagreement between the 6-ply
   evaluator and the 10-ply judge: an evaluator-bias floor, not sampling
   noise.
2. **Checkpoint features add nothing.** State-only RMSE 0.002519 versus
   checkpoint RMSE 0.002516. If the residual were sampling regret, BAI
   statistics would have to help. They do not, because the dominant label
   component is the bias floor, invisible to within-search statistics.
3. **Event strata match depth-bias strata.** Positive rows concentrate at
   bag 16--35 (lookahead horizon effects strongest before the endgame),
   PlayChooser trajectories (sharper positions), and near-tie counts
   (27/64 positive with >=2 near-ties versus 11/530 with none, a 20x lift).
   These are exactly the positions where a deeper evaluator reorders
   near-tied moves.

Absolute regret against the deep judge therefore mixes (a) a component the
next chunk can reduce (sampling/selection within the current arm) and (b) a
component it cannot (depth bias). The reliability gate keeps failing because
it demands decile calibration of an irreducible component. The decision only
needs (a): the regret reduction the remaining computation will actually
deliver.

## Proposal A: retarget to realizable chunk value via incumbent-survival hazard

1. **Hypothesis.** The decision-relevant quantity
   `V(t) = E[judge_regret(incumbent at t) - judge_regret(choice at horizon H)]`
   factorizes as `P(incumbent changes by H | trace <= t) * E[signed
   improvement | change]`. The first factor has free labels in every
   cumulative trace (the trace records whether the incumbent survived), is
   event-rich, and is predictable from entrenchment features. The second is
   sparse but poolable.
2. **Addresses the failure because** the corrected prospective panel's 96/96
   stopped-choice agreement shows survival is highly predictable at the
   conservative end, and if the incumbent survives to H the chunk-value
   difference is exactly zero by construction (same move, same judge value).
   The bias floor drops out of the stopping price entirely.
3. **Target/rule.** Discrete-time survival: hazard
   `h_k = P(incumbent changes during checkpoint k+1 | history)`;
   `P(change by H) = 1 - prod_{k<H}(1 - h_k)`, composable for any horizon and
   portable in iterations/nodes. Continue while
   `P(change by H) * E[improvement | change, coarse stratum] / chunk_seconds
   > lambda` (lambda from the water-filling dual), subject to existing
   floors. The improvement term is signed: continuation can talk the search
   out of the right move (the pair-4 PEG root).
4. **Runtime features.** Iterations since last incumbent change (log), lead
   changes so far, near-tie count (already logged in shadow),
   best-challenger gap/sigma trajectory and slope, fraction of horizon
   consumed, bag, candidate count, ply. Leakage risks: survival labels are
   hindsight within the trace (allowed for labels; no feature may reference
   post-k trace content); horizon must be an explicit conditioning variable,
   never implicitly the 3M cap; the judge stays label-only.
5. **Training/validation.** The conditional panel alone has 160,704
   checkpoint records; add the 120-root optional-stopping and 96-root
   prospective panel traces. Every incumbent flip is a labeled event at zero
   oracle cost; unlabeled traces can be minted from cheap autoplay.
   Discrete-time hazard (logistic or boosted) with the existing
   five-outer/four-inner complete-game folds. `E[improvement | change]` from
   judged panels at checkpoints where incumbent differs from the horizon
   choice (both moves are in the risk-set union), pooled with hierarchical
   shrinkage over policy x bag x near-tie cells.
6. **Smallest decisive experiment.** Offline: (i) held-out survival
   reliability by decile (hundreds of change events make this a real test);
   (ii) replay the stop rule on existing judged panels over a frozen lambda
   grid. Inference unit: source game. Pass: survival slope in [0.7, 1.4]
   with all deciles <= 2x, and replayed savings >= 25% of nodes at
   added-regret upper CI <= the 0.001 noninferiority margin. Only then one
   fresh prospective stopped/matched/full panel.
7. **Effort/compute.** ~1 week: trace-relabeling script, hazard fit beside
   `fit_sim_checkpoint_regret.py`, replay hooks into
   `analyze_regret_stopping.py`. Offline compute negligible; no new oracle
   data until the single prospective panel.
8. **Main failure mode.** Dependent censoring: high-hazard roots may have
   systematically different severity, underpricing dangerous strata. Test
   severity homogeneity across hazard quintiles before pooling.
9. **Abandon if** held-out survival reliability fails by deciles despite
   abundant events, or severity varies >5x across strata too sparse to
   condition on.

## Proposal B: game-level conformal risk control (CRC) stop set

1. **Hypothesis.** The q95 bound is 17.8x too conservative because it
   controls a per-row residual quantile. The controller needs
   `E[total missed regret per game] <= alpha`. CRC over the nested family
   `{stop iff score <= tau}` certifies exactly that, distribution-free under
   game exchangeability, and needs only ranking quality, not calibration.
   The failed point model may be a usable ordering.
2. **Addresses the failure because** mean actual positive regret is ~0.0016
   per game; a budget of alpha = 0.00025--0.001 tolerates the aggregate tail
   without decile calibration.
3. **Rule.** On calibration games compute per-game clipped missed regret
   `L_g(tau) = min(sum of stopped-row actual regret, B)`; choose the largest
   tau with `(n * Rbar(tau) + B) / (n + 1) <= alpha`. Caveat: unclipped
   B ~ 1 with n ~ 192 makes the correction ~0.005 and unusable; control
   clipped regret at B = 0.05 and separately report the empirical rate of
   events exceeding the clip. Both numbers go in the audit.
4. **Features/leakage.** Any nonanticipating score. Exchangeability requires
   calibrating within trajectory-policy strata, since the mix is a design
   choice.
5. **Training/validation.** Purely offline on the existing 844-row/192-game
   held-out set.
6. **Smallest decisive experiment.** CRC thresholds at
   alpha in {0.00025, 0.0005, 0.001}/game using (a) the current failed model
   and (b) proposal A's score. Pass: >= 2x the stop coverage of q95 at equal
   or lower realized missed regret. Zero new compute.
7. **Effort.** 2--3 days.
8. **Main failure mode.** Poor ranking: events not concentrated in
   high-score rows collapses coverage back to ~6% savings.
9. **Abandon if** CRC at alpha = 0.001/game stops <10% of rows under both
   scores.

## Proposal C: hurdle upgrade with hierarchical pooling and severity mixture

The existing conditional model already predicts event probability and
severity separately; the delta is in the pieces.

1. **Hypothesis.** Severity is bimodal: spread-scale misses (~1e-4) and
   win-class flips (~1e-2). The 43.97 worst ratio is a win-flip in a bin
   priced at spread scale. With 68 events, severity must be pooled
   hierarchically; the one conditioning worth its degrees of freedom is
   win-flip risk.
2. **Addresses the failure because** decile blowups are rare large-severity
   events in bins whose mean is set by small events.
3. **Target.** `E[regret] = P(event) * [pi_flip * mu_flip +
   (1 - pi_flip) * mu_small]`, mu components pooled globally with shrinkage
   toward parents, consistent with the monotone-table philosophy.
4. **New feature.** Win-component disagreement among near-tied challengers:
   for arms within utility-epsilon of the incumbent, the spread of their
   win-probability estimates. High values mean utility ties mask win/spread
   tradeoffs, exactly where a deeper judge flips the win class (mirrors the
   endgame settling result at the win/tie boundary). Nonanticipating,
   available from BAI stats.
5. **Training/validation.** Refit on the same 844 rows and folds; same
   preregistered reliability gate. Posterior predictive check: does the
   mixture reproduce the observed severity histogram including the outlier?
6. **Smallest decisive experiment.** Offline refit and gate; no new data.
7. **Effort.** 3--4 days.
8. **Main failure mode.** 68 events too few to identify a mixture.
9. **Abandon if** posterior predictive checks show the mixture
   unidentifiable or deciles still fail; then magnitude prediction at this
   event count is dead and A + B is the only viable stack.

## Proposal D: nonparametric joint arm distribution from covariance probes

1. **Hypothesis.** Clark joint-Gaussian fails conditional calibration not
   from covariance (empirical covariance zeroed bias and cut MAE 19.3% but
   worsened NLL) but from shape: the win component of blended utility is
   near-Bernoulli, so near-tie differences are skewed mixtures. Bootstrap
   resampling of the stored 512-common-scenario x 8-play probes estimates
   `E[(max_j U_j - U_inc)+]` and `P(incumbent not best)` without Gaussian
   assumptions.
2. **Why it matters.** Cheapest decisive discriminator between the two
   explanations of miscalibration. Fixing the deciles revives the
   within-search-statistics route; failing confirms the evaluator-bias floor
   and the retarget (A) plus escalation (E).
3--5. **Rule/features/training.** Offline recomputation only: resample
   scenarios per stopped-time checkpoint, recompute joint shortfall, compare
   against judge labels with the existing decile machinery in
   `analyze_regret_estimators.py`.
6. **Smallest decisive experiment.** Existing 120-root panel. Pass: fixes
   >= half the >2x deciles relative to the Gaussian empirical-covariance
   variant. Inference unit: source game.
7. **Effort.** ~2 days, zero new compute.
8--9. **Failure/abandon.** If no better, permanently retire "better
   within-search statistics" as a direction; that negative result is worth
   two days.

## Proposal E: depth-escalation rescue probe (high-upside unconventional)

1. **Hypothesis.** The dominant purchasable regret is not bought with more
   same-ply nodes. When the incumbent is stable but near-ties persist, only
   deeper evaluation of the near-tie set changes the answer. A targeted
   head-to-head probe (incumbent plus 1--3 challengers, deeper ply, fixed
   iteration budget) converts the failure stratum into near-zero regret at a
   fraction of a full arm's cost. The action space becomes
   {stop, continue, escalate}.
2. **Evidence.** p6@300K (0.000233) beats p4@10M (0.000661): depth dominates
   nodes. p6 3M-to-10M changed 0/80 choices: same-ply continuation is nearly
   worthless once stable. Events concentrate where the deeper judge disagrees
   with the 6-ply search. The pair-4 PEG root is the same phenomenon in
   another mode.
3. **Rule.** At a checkpoint with stable incumbent and >=1 near-tie, if the
   same-ply hazard-priced value is below lambda but near-tie structure
   predicts bias risk, spend a fixed pre-priced probe (e.g., 2--4 nominees x
   20K common-seed samples at 10 ply), adopt the probe winner, then stop.
   Interruptible, fixed-size, priced in portable iterations; satisfies the
   completion-gate rules trivially.
4. **Features/leakage.** Same nonanticipating checkpoint set as A plus the
   win-flip disagreement feature from C. The probe is runtime work, not a
   label.
5. **Training/validation.** Judged panels already contain the escalation
   oracle (10-ply/100K judge over the risk set). Offline, compute the value
   escalation-instead-of-continuation would have captured at each checkpoint
   and its hypothetical probe cost.
6. **Smallest decisive experiment.** Two stages: (i) offline replay: is
   capturable escalation value >= 3x capturable continuation value in the
   trigger stratum? (ii) a small prospective probe-fidelity panel: does a
   20K-sample probe agree with the 100K judge on change events >= 70% of the
   time? Inference unit: source game.
7. **Effort.** 1--2 weeks; the `thinkingcurve` forced-judge machinery does
   most of what a probe needs.
8. **Main failure mode.** Probe noise misranks near-ties, adding a new error
   source; probe cost tails in pathological positions.
9. **Abandon if** offline replay shows capturable escalation value < 2x
   continuation value, or probe-judge agreement < 70%; then depth bias is
   real but not cheaply purchasable at runtime, and the honest response is
   B's risk budget plus wider default ply.

## Rankings

Best three by expected progress per unit time and compute:

1. **A** converts ~95% of label mass from oracle-expensive to free, aligns
   the target with the decision, reuses every existing trace, and its first
   gate is entirely offline.
2. **B** is 2--3 days, zero compute, and is the certification layer that
   lets an uncalibrated-but-well-ordered score ship safely. It also rescues
   value from the already-failed model.
3. **D** is two days, zero compute, and decisive in both directions.

C is fourth: useful for the magnitude price water-filling withdrawals
eventually need, but 68 events is thin and A + B do not require it.

**High-upside unconventional:** E. If the plateau diagnosis is right,
{stop, continue} is the wrong action space and every predictor improvement
is optimizing within it.

**Simple controller deployable sooner (Rule of Zero):** stop at a checkpoint
iff (near-tie count = 0) AND (incumbent unchanged >= K checkpoints) AND
(work >= existing floor); otherwise run to the slice. The near-tie counter
is already logged in production shadow; the zero-near-tie stratum has a 2.1%
event rate before conditioning on stability. Tune K with B's CRC machinery
on existing data. Needs no calibrated magnitudes, fails closed on missing
statistics, and is a strict extension of the frozen-target rule that already
passed a prospective panel with zero misses.

## Two-week plan

- **Days 1--2:** Run D on the existing 120-root probes; write the
  trace-relabeling script for A. Stop condition: if bootstrap fixes the
  deciles, promote a bootstrap-based stopping price into the week-2 freeze
  and demote A to backup.
- **Days 3--5:** Fit A's discrete-time hazard on existing folds; test
  severity homogeneity across hazard quintiles; fit pooled
  improvement-given-change. Stop condition: survival reliability fails by
  deciles, abandon A, promote Rule of Zero.
- **Days 6--7:** Implement B; calibrate CRC thresholds at
  alpha in {0.00025, 0.0005, 0.001}/game for the hazard score and the
  current model's score on the 192 held-out games; offline replay of the
  composed rule. Stop condition: <15% replayed node savings at every alpha
  means the week-2 panel is not justified; ship Rule of Zero toward the
  terminal-game gate and pivot research to E.
- **Days 8--9:** Freeze and preregister: score definition, CRC threshold,
  lambda grid, horizon conditioning, gates (>= 25% node savings;
  game-clustered added-regret upper CI <= 0.001; survival reliability slope
  in [0.7, 1.4] with all deciles <= 2x). Add the outside-top-60 rider: judge
  two importance-sampled candidates from ranks 61--200 per root (~2% judge
  overhead) so the unmeasured tail accumulates an unbiased bound across
  panels.
- **Days 10--14:** One fresh 96-root prospective stopped/matched/full panel
  under the corrected protocol, fresh games, strict audit. In parallel
  (offline, free): E's replay quantification of capturable escalation value.
- **Throughout:** live allocation stays off; the mirrored terminal-game gate
  remains separate and mandatory even on a surrogate pass; all inference
  stays game-clustered; everything fails closed on missing statistics.

## Recommended first step

Run D first as a two-day gate, then A, with B as its certification layer. D
is the cheapest experiment discriminating between the two live explanations
of five consecutive calibration failures. A is the only proposal that
changes what is being predicted rather than how hard it is predicted, and
its training data already exists in bulk. Retargeting to realizable chunk
value, with CRC as the safety certificate instead of decile calibration, is
the shortest path from honest-but-stuck to honest-and-shipping.
