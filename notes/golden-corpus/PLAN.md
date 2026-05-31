# Golden-corpus plan — resumption notes

Last updated: 2026-05-31. Branch: `nested-sim`. Paused to work on `magpie_tui`
until preendgame (PEG) merges — nested sims interacting with PEG is the
real target, so this corpus work is in a holding pattern.

## Where we are now

**Built and unused:**

- `goldenbuild` mode in `test/inner_agreement_test.c` — nested-only outer
  sim per position, `disable_similarity=true`, no heatmap. Outputs per-arm
  (move, win_pct, sample count) + per-ply (score_mean, score_count,
  bingo_rate) as one JSONL line per position. Env: `INNERAGREE_GOLDEN_BUILD_CSV`,
  `INNERAGREE_GOLDEN_BUILD_OUT`, `INNERAGREE_TLIM`.
- `goldeneval` mode — read a golden JSONL, run a strategy at each position,
  emit per-position CSV with `loss_vs_golden`. Env:
  `INNERAGREE_GOLDEN_EVAL_GOLDEN`, `INNERAGREE_GOLDEN_EVAL_OUT`,
  `INNERAGREE_GOLDEN_EVAL_STRATEGY=flat|nested`, `INNERAGREE_TLIM`.
- `notes/golden-corpus/aggregate_golden_eval.py` — single-strategy summary
  plus paired-t-test comparison of two strategies' loss CSVs.
- `notes/golden-corpus/README.md` — workflow docs.

**Both test files now use CSW24** (CSW21 lexicon + RIT files deleted to
free disk). `CSW24.rit` rebuilt at 1.88 GB.

**No corpus has been built yet.** No goldens, no eval CSVs. The framework
exists but is untested end-to-end past compile.

## Why we paused

PEG (preendgame) lookups in MAGPIE are a known win on endgame positions.
Nested sim's interaction with PEG is the real prize: at late-game
positions the nested sim's leaf eval should call into PEG instead of
either flat-equity or running another inner sim. Without that interaction
implemented, comparing nested vs flat in the late game just tests how
each handles the *absence* of PEG — not very interesting.

So: pause corpus build until PEG-aware nested sim is mergeable, then
resume.

## Immediate next steps when picking this back up

1. **Confirm PEG is wired into nested's leaf eval.** Currently `get_top_nested_sim_move`
   in `src/impl/random_variable.c` calls `endgame_ply_solve` when bag is
   empty, but `endgame_ply_solve` itself is just `get_top_equity_move`
   (TODO comment in the code). Real PEG integration would replace this.
2. **Build a positions CSV.** ~200 positions from `notes/inneragree-positions/csv/inneragree_perroot_v2.csv`
   balanced across game stage. Suggested buckets:
   - Opening (turns 0–5): 30 positions
   - Early mid (turns 6–12): 50 positions
   - Late mid (turns 13–18): 60 positions
   - Late game (turns 19+, bag<20): 60 positions (this is where PEG matters)
   - Cross-stratify by mean_loss (top quartile vs bottom quartile) to get
     diverse "spiciness"
3. **Run goldenbuild at 30 min/position:** ~100 hours on the Mac mini.
   Expect ~30 MB output.
   ```bash
   INNERAGREE_GOLDEN_BUILD_CSV=positions_200.csv \
   INNERAGREE_GOLDEN_BUILD_OUT=golden_200_30min.jsonl \
   INNERAGREE_TLIM=1800 \
   ./bin/magpie_test inneragree
   ```
4. **Eval cheap strategies and compare.** First experiment to settle:
   does HYBRID at 30s/turn actually beat (or match) PURE FLAT at 30s/turn
   on per-pick loss vs golden? We never reached significance via game-pair
   A/B (best result: 83 pairs, z = −0.97 on spread). Aggregate-against-golden
   on 200 positions is way more powerful — paired-t at z=2 needs ~100 evaluable
   positions vs ~400 game pairs.

## Open design questions to resolve before extending

### Q1: Replay or aggregate?
Current goldenbuild captures only aggregate per-arm stats. That supports
"score strategy pick against golden's top" but **not** subsampling
(simulate "strategy X with budget B" by drawing N rollouts from the corpus).

For replay we'd need per-rollout traces. Compact format: 4 bytes per
rollout (2-byte spread + 1-byte bingo bit + 1-byte plies). For 200
positions × ~500K rollouts/position avg = ~400 MB. Doable.

**Recommendation**: ship aggregate-only first. Extend to replay only if
aggregate analysis points to a question that needs it. Common questions
(does X beat Y at same budget? what's the loss curve over budget?) are
answerable with aggregate + cheap eval reruns.

### Q2: How to validate golden quality?
Golden has known biases (inner sim's max-of-K leaf eval, possible
similarity-prune artifacts even with `disable_similarity=true`). It
measures "vs golden's view of truth", not "vs ground truth."

For partial validation: run goldenbuild twice on a small held-out subset
with different RNG seeds, only keep positions where both goldens agree on
the top arm. Catches positions where even golden is unstable.

### Q3: Position selection — random or curated?
Random positions from self-play will be representative but mostly boring
(positions where flat = nested). Curated to pre-screen disagreement
positions will give more statistical power but skew the corpus toward
hard cases.

**Recommendation**: 60/40 mix. 60% random from per-root CSV, 40% from the
pick-disagreement subset (92 positions in `inneragree_pickdiff.csv`).

### Q4: Train/test split methodology
Mentioned but not implemented. Once corpus exists:
- 80/20 split by position ID hash (deterministic, reproducible)
- Tune thresholds / inner-sim params on train
- Report final numbers on test
- Optionally cross-validation if corpus is small

## PEG-aware nested sim — the actual prize

The interaction between nested and PEG is what we think will matter most.
Three concrete bets:

1. **Late game (bag ≤ 14)**: PEG should replace flat-equity leaf eval.
   Should drop nested's "endgame is just flat" fallback (currently in
   `endgame_ply_solve`) and have it call into PEG. Expect noticeably
   better picks on close late-game positions where leaf evaluation
   determines the outcome.
2. **Pre-PEG transition (bag 15–25)**: this is the "midgame where nested
   helps" zone we've been measuring. Closed-board geometry combined with
   short rollout windows. Nested + flat-eval-at-leaves vs nested +
   PEG-eval-at-leaves should produce different rollouts.
3. **Probe signal**: with PEG-aware leaf eval, the inner-sim's mean_loss
   probe metric we used in HYBRID may behave differently. Re-evaluate
   what the right routing signal is once PEG is wired in.

## What to commit/push when resuming

Code state is already pushed (commits `589286eb` and `8977a520`). On
resume:
1. `git pull origin nested-sim` (or whatever branch PEG-aware-nested
   lands on)
2. Rebuild: `make magpie_test BUILD=release`
3. Verify CSW24 still default (or update test configs again)
4. Resume from "Immediate next steps" above

## Files to look at

| file | purpose |
|---|---|
| `test/inner_agreement_test.c` | goldenbuild + goldeneval modes |
| `src/impl/random_variable.c:451` | `get_top_nested_sim_move` — the inner sim. Look for the `bag_is_empty` branch when wiring PEG. |
| `src/impl/random_variable.c:584` | `endgame_ply_solve` — the placeholder PEG hook |
| `src/def/bai_defs.h` | `BAIOptions.disable_similarity` |
| `notes/inneragree-positions/csv/inneragree_perroot_v2.csv` | starter position pool |
| `notes/golden-corpus/README.md` | usage docs for the modes |
| `notes/golden-corpus/aggregate_golden_eval.py` | scoring script |
