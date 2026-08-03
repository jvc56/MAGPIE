# Rule-of-Zero terminal-game gate: review packet

Status: **draft for adversarial review; not a preregistration and not an
authorization to launch a pilot.** Live allocation remains disabled.

This packet responds to the terminal-gate review after the frozen
Rule-of-Zero surrogate result in
`obj/rest-game-stratified-panel-20260801/rule-zero-prospective-320-20260803`.
It deliberately does not change the frozen rule:

```text
stop iff nodes >= 100,000
      and stable_checkpoints >= 2
      and near_tie_challengers == 0
otherwise run to the ordinary slice
```

The cross-fitted score remains shadow-only.  The reused-outcome `0.000025`
threshold is not a decision input, and no CRC rule is introduced here.

## 1. Blocking topology finding

The surrogate pass was for a specific search topology.  The current
production PlayChooser path is not that topology.

| Field | Frozen 320-root surrogate | Current PlayChooser default |
| --- | --- | --- |
| SIM plies | 6 | 2 |
| candidate screen | statically sorted top 60 | top 15 |
| sampling floor | 10% uniform floor | 32 samples per arm |
| checkpoint cadence | 256 requested iterations | no stopping observer |
| horizon | fixed 3M nodes | current time-budgeted slice |
| threads in the experiment | 10 | supplied by the match runner |

The default facts are source-level facts: `src/impl/play_chooser.c` uses the
default p2/top-15 path and its 32-sample prefix when no explicit strategy is
provided.  The existing match runner does not install a Rule-of-Zero observer.
Therefore the 1/320 result (one-sided 95% mismatch upper bound 1.4738%) **must
not be transferred to a terminal match as-is**.

Before a terminal pilot, the project must do one of the following:

1. validate Rule of Zero in a new judge-light panel at the exact current
   production p2/top-15/min-32/256-checkpoint topology; or
2. separately establish p6/top-60 as the production baseline for *both* arms,
   then validate that baseline change before testing the stop flag.

Option 1 is preferred because the terminal control must be today's production
default and treatment must be exactly `control + Rule-of-Zero flag`.

The first option needs a real production implementation with the observer
disabled by default.  The implementation must fail closed on a missing,
negative, or malformed near-tie counter, retain the normal time limit when it
does not stop, and have a regression test that an invalid counter cannot end a
search.  The 256-iteration cadence is part of the policy identity, not
diagnostic logging.

## 2. Proposed horizon treatment after topology validation

The recommended terminal treatment is **uncapped**: Rule of Zero may stop a
SIM search early, otherwise that turn receives the exact ordinary production
time slice.  It never extends a search.  This avoids deliberately capping
every live slice at 3M nodes when normal 3-minute slices have historically
received roughly 3.65M--6M nodes.

The old p6/top-60 evidence is useful only as a disclosed sensitivity result:

- stop versus 3M mismatch upper bound: 1.4738%;
- p6 3M-to-10M choice-drift upper bound from the independent 0/80 panel:
  3.68%;
- their simple union-bound sensitivity is 5.1538%.

It is **not** a validity bound for p2/top-15. The exact-topology panel above
must instead run each cumulative arm through its **actual per-root live
equal-slice receipt**, with the historical minimum of 3.65M nodes as the floor
for the panel's horizon. This kills the prior horizon extrapolation rather
than attempting to bound it. Each root logs its receipt and the corresponding
horizon choice. The panel remains judge-light: judge every mismatch plus the
preregistered random audit sample. Only a pass at that actual-live landmark
may carry a stop-rule claim into the terminal pilot. A fixed 3M-capped
treatment is rejected here because it changes every terminal-game SIM decision
and does not test the intended live controller.

## 3. Primary terminal estimand and design

The terminal experiment remains a common-RNG, mirrored, two-game pair design
using the existing `tools/run_time_manager_match.py` protocol and its detailed
turn/trace audit.  A pair is the inference unit.

- **Control:** current production TimeManager: spend-down bridge enabled,
  PEG allocation shadowed, otherwise unchanged.
- **Treatment:** the same binary and configuration with only the disabled-by-
  default Rule-of-Zero flag enabled for the treatment player.
- **Primary endpoint:** pair-level terminal blended-utility difference,
  treatment minus control, tested for one-sided noninferiority at fixed margin
  `delta_U = 0.020`.
- **Primary analysis:** a one-sided 95% complete-pair Student-t lower bound
  must exceed `-0.020`, and the point estimate must be at least `-0.010`.
  There is no superiority claim.
- **Outcome-based stopping:** prohibited.  The confirmatory sample size is
  fixed after a separate variance pilot.  Operational aborts (build/hash,
  trace accounting, penalty safety, or hardware failures) are reported
  separately and never converted into an outcome-based success/failure rule.

`delta_U = 0.020` is deliberately not selected from the 72-pair mean. This
terminal gate is a systemic-failure detector: the direct stopping channel is
already surrogate-bounded at roughly `0.0003`--`0.0005` utility per game, too
small to power directly, whereas the known systemic failures (pair 45, pair
68, and the five-pair `-0.163` scare) are at the `0.01+` scale. The margin is
therefore designed to detect those failures while retaining a feasible paired
experiment; it is not a claim that a 0.020 utility loss is desirable.

## 4. Empirical power calculation from the prior terminal match

The killed 72-pair 3-minute match is used for variance only.  Its pair-level
terminal-utility difference had sample SD `0.17147`; its spread difference had
SD `36.667` points.  The historical utility mean (`-0.00744`) is not used to
pick a direction or a margin.

The table is the normal-approximation planning calculation for a one-sided
5% noninferiority test,

```text
n = ceil(((z_0.95 + z_power) * s_pair / delta_U)^2)
```

using `s_pair = 0.17147`.  The final calculation must be rerun from the
excluded pilot's pair SD, preferably using its upper confidence limit as a
conservative planning SD.

| Utility margin `delta_U` | approximate spread equivalent near a tied game* | pairs for 80% power | pairs for 90% power |
| ---: | ---: | ---: | ---: |
| 0.0025 | 3 points | 29,086 | 40,288 |
| 0.0040 | 5 points | 11,362 | 15,738 |
| 0.0050 | 6 points | 7,272 | 10,072 |
| 0.0100 | 12 points | 1,818 | 2,518 |
| **0.0200 (frozen)** | **24 points** | **455** | **630** |

\* Holding win probability at 50%, the configured blended utility has local
spread slope about `1/1200` per point.  This is only an interpretation aid,
not a replacement endpoint.

For comparison, the same historical spread SD needs about 333 pairs for 80%
power or 461 pairs for 90% power at a 5-point spread noninferiority margin.
That is why a terminal-utility primary is much more demanding.

The proposed variance pilot is **30 mirrored 3-minute pairs**.  It uses the
same topology and policy arms as the later match but is permanently excluded
from the confirmatory analysis.  It exists to measure the current pair-level
utility variance, operational reliability, and achieved Rule-of-Zero stop
rate.  It does not authorize changing the rule, the utility margin, or the
terminal endpoint after outcomes are viewed.

The confirmatory sample size is frozen after the pilot as

```text
N = ceil(8.57 * (SD_pilot / 0.020)^2)
```

for one-sided `alpha=0.05` and 90% power, with `N` capped at **800 complete
pairs**. If the formula exceeds 800, the terminal gate is infeasible at the
chosen margin and this candidate does not ship; the cap is not an invitation to
reduce power. The historical SD would give 630 pairs, but it came from a
different-duration match and is used only as a planning reference.

There is one formal interim look at approximately half the frozen information
(`ceil(N/2)`, 315 pairs if `N=630`) using one-sided O'Brien--Fleming alpha
spending. At half information its nominal one-sided alpha is approximately
0.010; the final cumulative alpha is 0.050. There is no outcome-based futility
or harm stop. An operational failure is a restart/not-a-result condition under
the frozen hash and trace protocol, never a favorable or unfavorable endpoint
stop.

The primary analysis is CUPED-adjusted with the unadjusted paired analysis as
a required sensitivity analysis. The covariate is the average of the two
seat-specific, treatment-blind pre-game control-policy terminal-utility
forecasts: it may use only the initial CGP/racks, starting seat, fixed seed,
clock, and frozen static/equal-slice features available before either game is
played. Its forecaster and the CUPED slope are fitted only on the excluded
pilot and prior calibration/control data, then frozen before the first
confirmatory pair. No confirmatory outcome may refit either object.

## 5. Secondary operational endpoints

These are not substitutes for the primary endpoint:

1. penalties and overtime: treatment must not violate the predeclared
   operational safety threshold relative to control;
2. PEG false starts: treatment must not add any; PEG allocation remains
   shadowed in both arms;
3. Rule-of-Zero stop rate, nodes/time saved, and SIM move disagreement;
4. reinvestment, measured only on shared-prefix turns before first divergence.

Reinvestment is **not terminal remaining clock**.  For each treatment SIM
turn `t`, the trace must record the ordinary counterfactual legacy slice
`L_t`, actual elapsed SIM time `A_t`, and scheduled budget `B_t`.  A saved
amount is `max(0, L_t - A_t)`.  It is counted as reinvested only to the extent
that a later, pre-divergence treatment SIM turn spends actual time above its
own legacy slice, `max(0, A_u - L_u)`.  Report

```text
reinvested_before_divergence =
  min(sum_t saved_t, sum_later_predivergence_u max(0, A_u - L_u))
```

along with planned-budget deltas.  Post-divergence values are descriptive only
and cannot establish reinvestment.

## 6. Informational miss autopsy

The one surrogate miss was source 26, a PlayChooser bag-71 root.  It stopped
at the first legal checkpoint (100,400 nodes), with rank 1 stable for 54
checkpoints and zero reported near ties; the 3M arm selected rank 0.  The
judge measured utility loss 0.00206846.

The stored Rule-of-Zero trace records the incumbent and a scalar challenger
estimate, but not the full checkpoint risk-set membership or challenger rank.
It can therefore establish that rank 0 was in the **final judge union** (the
judge compared ranks 0 and 1), but it cannot distinguish whether rank 0 was
outside the risk set at the stopping checkpoint from whether its variance was
underestimated.  A diagnostic replay of this archived root may add that
telemetry, but it is informational only: it must not change the frozen rule or
be used to amend the surrogate result.  The new exact-topology panel must log
the stopping-checkpoint risk-set membership—the normalized ranks/IDs in the
risk set and a `full_horizon_rank_in_risk_set` indicator—so this ambiguity
cannot recur.

The miss happened at the first legal work checkpoint.  With only one event,
there is no reliable estimate that first-eligible stops are overrepresented;
the exact-topology panel will report that predeclared stratification.

## 7. Conditions before this draft becomes a frozen pilot

1. **Completed locally; pending this packet's line review.** The
   disabled-by-default p2/top-15 observer now lives in the BAI loop, with a
   256-iteration work-coordinate cadence.  It is enabled only by the
   benchmark/match flag `PCBENCH_RULE_ZERO_P0_ONLY`; an absent flag leaves the
   existing path on its ordinary solver boundary with no Rule-of-Zero exit. A malformed Rule
   Zero configuration (including a missing native-work counter) is inert, and
   a missing/negative near-tie diagnostic cannot stop.  `PCTURN` records the
   enabled/stopped state, stop work, iteration, stability, switch, near-tie,
   and the final selected candidate rank plus move fingerprint.  The latter is
   explicitly distinct from any raw BAI arm index.  Regressions cover a legal
   stop, a missing work counter falling through to the ordinary sample
   boundary, and a nonzero near-tie continuing to that boundary.  The
   forthcoming exact-topology harness must additionally normalize and record
   checkpoint risk-set membership; that is panel telemetry, not an excuse to
   alter this frozen rule.
2. Freeze and complete the fresh exact-topology, judge-light mismatch panel
   through the per-root actual-live equal-slice landmark (at least 3.65M
   nodes). It must rederive p2/top-15 stop rate, savings, near-tie counts,
   mismatch, and missed-value summaries under the existing 1.5% and 0.001
   gates. A failure ends this candidate; it does not trigger a post-hoc rule
   adjustment.
3. Freeze the terminal pilot manifest, hashes, operational thresholds, and
   the variance-pilot sample size.  Run the pilot only after this packet has
   received the requested adversarial review.
4. Freeze the confirmatory `N` using the pilot's excluded variance estimate;
   then run the mirrored terminal match.  A surrogate and terminal pass still
   precede any production enablement.
