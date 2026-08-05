# Rule-of-Zero exact-topology (p2/top-15) panel — preregistration DRAFT

Status: **draft; freeze before any trajectory generation**. Two fields are
deliberately unfrozen and must be filled from the match machine before
collection: the worker thread count and the confirmation that the arm's
per-play sampling floor matches production PlayChooser's 32-sample prefix.
Everything else is fixed now so freezing is a fill-in, not a redesign.

## Rule under test (unchanged from the frozen surrogate)

```text
stop iff nodes >= 100,000
      and stable_checkpoints >= 2
      and near_tie_challengers == 0
otherwise run to the ordinary slice
```

Harness/observer parity for the checkpoint cadence (256 completed BAI
samples) and the stability semantics is verified by inspection and recorded
in `notes/TIME_MANAGER.md` ("Rule-of-Zero observer hardening"). The panel
uses the harness's cumulative-arm replay; the production observer's shadow
mode (`PCBENCH_RULE_ZERO_SHADOW`) is smoke-tested separately on PCTURN
traces and must agree with the harness rule on any overlapping trace before
the terminal pilot.

## Topology

- 2 plies, statically sorted top-15 candidates, production 32-sample
  per-arm prefix (**verify**: the arm must use `sample_minimum=32`, not the
  p6 panel's 10% uniform floor), 256-sample checkpoints.
- Threads: **fill from the match machine's `run_time_manager_match.py`
  configuration** before freezing; the panel and the terminal match must
  use the same worker count.
- Cumulative arm capped at a fixed **8M-node superset**; no judge except
  mismatches and the audit subset.

## Corpus

320 fresh complete-game roots, one per game, 40 per trajectory-policy
(static / PlayChooser-g3000ms) by bag-band (`<=15`, `16--35`, `36--60`,
`61--100`) stratum. Exclude every game in the 80-root risk-set, 120-root
optional-stopping, 96-root combined-regret, and 320-root p6 prospective
manifests. Panel order by the selection tool's frozen SHA-256 rank; indices
0 mod 3 receive exact-iteration matched arms; indices 0 mod 10 form the
random judge audit.

## Landmarks and estimand

Mismatch is scored against three logged landmarks from the same trace:
3.65M (historical minimum live equal-slice receipt), 6M (historical
median), and 8M (the cap). The **gate applies at 6M**; the other two are
mandatory reported sensitivities. The stopping-checkpoint risk-set
membership and a `full_horizon_rank_in_risk_set` indicator are logged for
every root so any miss is attributable (the bag-71 lesson).

## Preregistered gates (all must hold; any miss rejects the candidate)

1. Exact one-sided 95% Clopper--Pearson mismatch upper bound <= 1.5% at the
   6M landmark.
2. Complete-game Student-t 95% upper bound on judged missed value <= 0.001
   at the 6M landmark.
3. **Usefulness floor**: the rule stops >= 30% of roots and saves >= 25% of
   nodes at the 6M landmark. A safety pass on a rule that almost never
   fires must not launch a 630-pair terminal match.
4. Matched-subset optional-stopping control: stopped-minus-matched judged
   value interval includes zero with upper endpoint <= 0.001.

Acceptance requires a contiguous 0--319 prefix, exact
stopped/matched/landmark joins, frozen subset membership, selective-judge
accounting, and `dropped=0`. The shadow score is logged and cannot affect
any decision. A pass clears only the surrogate gate; the separately
preregistered mirrored terminal-game experiment (see
`notes/RULE_ZERO_TERMINAL_GATE_REVIEW_PACKET.md`) remains the only path to
live enablement.

## Invocation (after freezing the two fill-ins)

```bash
python3 tools/run_rule_zero_prospective_pipeline.py \
  # ... prepare/select steps per the p6 pipeline, new exclusion manifests
python3 tools/run_rule_zero_panel.py \
  --plies 2 --num-plays 15 --max-nodes 8000000 \
  --equal-slice-nodes 6000000 --threads <MATCH_THREADS> \
  --minimum-nodes 100000 --minimum-stable-checkpoints 2 \
  --checkpoint-interval 256 --matched-modulus 3 --audit-modulus 10 \
  --judge-plies 10 --judge-samples 100000 --judge-risk-plays 8 \
  # --uniform-floor-per-mille replaced by the verified 32-sample prefix
```

The 3.65M and 8M landmark scores are computed by the analyzer from the
recorded checkpoints; no additional arms are required.
