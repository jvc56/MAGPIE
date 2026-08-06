# Rule-of-Zero certification: primary-gate decision

Decision date: 2026-08-06. Decided by the owner; recorded verbatim:

> The primary gate is judged-missed-value at the 0.001 threshold;
> mismatch-count is reported as a secondary.

## Scope and application

- Applies **prospectively** to the fresh exact-topology judge-light panel
  required by RULE_ZERO_TERMINAL_GATE_REVIEW_PACKET.md section 7.2, and to any
  later certification stage. The completed p2 pathfinding panel
  (notes/rule_zero_p2_pathfinding_results.json) is prior evidence, not a pass:
  it ran under the earlier dual gate and failed only the mismatch upper bound
  (3/320 = 0.94%, one-sided 95% upper 2.4% > 1.5%) while its judged missed
  value was mean -1.33e-05 utility with 95% upper bound +1.49e-05 - 67x inside
  the 0.001 gate. Re-scoring that same panel under the new primary would be
  post-hoc; the fresh panel decides.
- Secondary (reported, not gated): landmark mismatch count with exact
  Clopper-Pearson bounds, unchanged definition.
- Rationale: judged evidence shows Rule-of-Zero mismatches are predominantly
  value ties (p2 panel judged-missed-value above; independently, oracle-judged
  endgame decision flips after 50% of solve cost were 0-point ties in 30/30
  cases). A count gate fails on harmless tie-swaps and can hide rare large
  misses inside an acceptable rate; the judged gate prices exactly the quantity
  certification protects.

## Sizing consequence

- The mirrored terminal match cannot be sized from terminal-utility variance:
  the 30-pair variance pilot returned pair SD 0.003176 (21/30 pairs exactly
  tied), making N = ceil(8.57*(SD/0.020)^2) = 1, i.e. the utility outcome test
  is near-vacuous under mirroring. Terminal-match sizing therefore follows the
  primary gate's statistics, not utility SD.
- Fresh exact-topology panel: retain the stratified 8x40 = 320-root design. At
  the p2 panel's observed missed-value distribution this carries ~67x margin on
  the 0.001 primary; the secondary imposes no sizing requirement.
- All other packet section 7 conditions (usefulness floor of >=30% stops and
  >=25% node savings at the 6M landmark, freezes, review) are unchanged.

## Applied by

- notes/rule_zero_exact_topology_preregistration.json — the fresh section 7.2
  exact-topology certification panel preregistration (fresh seeds 8404001 /
  8504001; primary judged-missed-value gate; secondary mismatch reported;
  usefulness floor at the 6M landmark; risk-set telemetry required at
  progress schema >= 7).
