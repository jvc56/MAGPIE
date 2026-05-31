# Golden-corpus framework

Offline experimentation infrastructure: build a corpus of positions with
per-arm wpcts computed at a very long nested budget (the "golden"), then
score cheap strategies by comparing their picks to the golden's top arm
(and the golden wpct of the pick they actually made).

Removes the need for full game-pair A/Bs for every threshold/probe tweak.

## Modes (all in `test/inner_agreement_test.c`)

### `goldenbuild` — build golden per-position JSONL
```
INNERAGREE_GOLDEN_BUILD_CSV=positions.csv \
INNERAGREE_GOLDEN_BUILD_OUT=golden.jsonl \
INNERAGREE_TLIM=1800 \
./bin/magpie_test inneragree
```
Input CSV: any CSV whose first 3 columns are `game,turn,on_turn_player` and
whose LAST column is `cgp`. (The `inneragree_perroot_v2.csv` format works.)

Per position, runs **nested-only** outer sim at `INNERAGREE_TLIM` seconds
with `disable_similarity=true` (epigons not pruned) and no heatmaps. Emits
one JSONL line per position:
```
{"game":N,"turn":N,"on_turn_player":N,"cgp":"...","tlim":T,"arms":[
  {"move":"...","win_pct":0.xxxxxx,"n":N,
   "plies":[{"s":score_mean,"sn":N,"b":bingo_rate}, ...]},
  ...
]}
```

### `goldeneval` — evaluate a strategy against the golden
```
INNERAGREE_GOLDEN_EVAL_GOLDEN=golden.jsonl \
INNERAGREE_GOLDEN_EVAL_OUT=eval_flat_30s.csv \
INNERAGREE_GOLDEN_EVAL_STRATEGY=flat \   # or "nested"
INNERAGREE_TLIM=30 \
./bin/magpie_test inneragree
```
For each position in the golden file, loads the CGP, runs the chosen strategy
at `INNERAGREE_TLIM` seconds, looks up the strategy's top pick in the
golden's arms, and writes a per-position CSV row:
```
game,turn,on_turn_player,strategy,tlim,
golden_top_pick,golden_top_wpct,strat_pick,
strat_wpct_in_golden,loss_vs_golden,agree,found_in_golden
```

- `loss_vs_golden` = `golden_top_wpct − strat_wpct_in_golden` (positive
  means strategy picked something golden values worse than its own top).
- `found_in_golden` = 0 when the strategy picked a move that the golden
  never sampled (mostly happens for low-equity moves the strategy
  considered but golden pruned away). Those rows have `strat_wpct_in_golden = -1`
  and `loss = -1`; they shouldn't be summed.

### `aggregate_golden_eval.py` — summary + paired comparison
```
python3 notes/golden-corpus/aggregate_golden_eval.py eval_flat_30s.csv
python3 notes/golden-corpus/aggregate_golden_eval.py eval_a.csv eval_b.csv
```
Two-arg form does a paired t-test on `loss(A) − loss(B)` over the
intersection of position keys where both strategies' picks were found in
the golden. The paired structure removes most position-to-position variance
and is way more powerful than independent game-pair A/Bs.

## Workflow

1. **Generate positions** (~200–1000):
   - Use the existing inneragree per-root run, or curate from screening
     CSVs (`inneragree_screen30s.csv` / `inneragree_nscreen30s.csv`).
   - Write a positions CSV with columns
     `game,turn,on_turn_player,...,cgp`.
2. **Build golden** with a long budget (30–60 min per position is the
   sweet spot; longer for the held-out test set). Disables similarity
   pruning so epigons each get sampled — required for reliable per-arm
   wpcts.
3. **Eval strategies** at much shorter budgets (1s, 10s, 30s, 60s, ...)
   to learn each strategy's loss curve vs budget.
4. **Paired comparisons** between strategies at the same budget to test
   hypotheses (e.g., "hybrid at 30s beats flat at 30s?").

## Caveats

- Golden is only as good as the long-budget nested sim. Inner sim biases
  (max-of-K leaf eval) will leak into golden's wpcts. The framework
  measures "strategies vs golden's view of truth," not "vs ground truth."
  Useful for comparing strategies but not for absolute calibration.
- For a true second opinion on golden quality, run goldenbuild twice with
  different seeds and only accept positions where the two goldens agree
  on the top arm. (Not yet automated.)
- Per-position evaluation discards the game-flow context — a position that
  matters more for actual gameplay (e.g., a turning-point decision) is
  weighted the same as a benign one. The framework measures average pick
  quality, not win impact directly. For win-impact, you still need
  game-pair A/Bs.
