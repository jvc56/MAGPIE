# Killer Draws: Adaptive Scenario Ordering for PEG Pruning

## Problem

The PEG solver evaluates candidate moves by iterating over all possible draw
scenarios (which tile(s) the mover draws from the bag). For each candidate, it
accumulates a win percentage across these scenarios and uses early cutoff to
prune candidates that can't possibly reach the current best.

Pruning effectiveness depends on scenario ordering. If the first few scenarios
evaluated happen to be wins, the running win percentage stays high and pruning
is delayed even for objectively bad candidates. Conversely, if losing scenarios
are evaluated first, the running win percentage drops quickly and pruning fires
early — often after evaluating only a fraction of the total scenarios.

Prior to this change, scenarios were ordered strictly by multiplicity (weight)
descending. This is a reasonable default — high-weight scenarios contribute the
most to the final result, so evaluating them first gives the most information
per unit of work. But it ignores the content of the scenarios: some draw
tiles/pairs consistently produce losses across many candidates, while others
are almost always wins.

## Design

### Core Idea

Borrow the "killer move" heuristic from game tree search. In chess engines, a
move that caused a beta cutoff at a sibling node is tried first at subsequent
nodes, because moves that refute one position tend to refute similar positions.

The analogous insight for PEG: a draw that produced a loss for candidate A is
likely to produce a loss for candidate B too, because the draws represent the
same underlying tile distribution — only the candidate move differs.

### Sort Key

For each draw scenario with multiplicity `w`:

```
killer_score = loss_rate * w
```

where `loss_rate` is the fraction of prior candidate evaluations in which this
draw was a loss. Scenarios are sorted by `killer_score` descending, with `w` as
tiebreaker.

This balances two factors:
- **Loss rate** prioritizes draws that are empirically likely to be losses.
- **Weight** breaks ties so that among equally-likely-to-lose draws, the
  highest-multiplicity ones come first (maximizing the impact on the running
  win percentage per evaluation).

When no killer data exists yet (first candidate), `loss_rate = 0` for all
draws, so the sort degrades to pure weight ordering — identical to the previous
behavior.

### Data Structure

```c
typedef struct {
  atomic_int losses;  // candidates where this draw was a loss
  atomic_int evals;   // candidates that evaluated this draw
} KillerDrawEntry;

typedef struct {
  KillerDrawEntry entries[MAX_ALPHABET_SIZE * MAX_ALPHABET_SIZE];
} KillerDraws;
```

Indexed by a canonical key:
- **1-bag (single tile `t`):** key = `t * MAX_ALPHABET_SIZE + t`
- **2-bag (unordered pair `{a, b}`, `a <= b`):** key = `a * MAX_ALPHABET_SIZE + b`

Ordered pairs `(t1, t2)` from endgame evaluation are mapped to their canonical
unordered form via `killer_pair_key(t1, t2)`.

The table is 2500 entries (50 * 50) at 8 bytes each = 20 KB. Negligible.

### Loss Classification

A draw is classified as a "loss" for a candidate using a binary criterion:

- **RecUpair (2-bag greedy recursive):** Each unordered pair produces an
  aggregate win rate across opponent responses and sub-scenarios. If
  `win_rate < 0.5`, it counts as one loss vote.
- **PegPairScenario (2-bag endgame):** Each ordered pair produces a single
  `mover_total` from the endgame solve. If `mover_total < 0`, it counts as
  one loss vote. Both orderings of the same unordered pair contribute
  independently (two data points per candidate per pair).
- **PegSingleScenario (1-bag endgame):** Each tile produces a single
  `mover_total`. If `mover_total < 0`, it counts as one loss vote.

Binary classification (loss vs. not-loss) was chosen over continuous weighting
for simplicity and because it mirrors the chess killer heuristic: the question
is "does this draw tend to refute candidates?" not "by how much?"

### Concurrency

All counters use `atomic_int` with `memory_order_relaxed`. Multiple threads
may update the same entry concurrently (different candidates evaluating the
same draw pair). Relaxed ordering is sufficient because:

- Stale reads are acceptable — killer stats are a heuristic, not a correctness
  requirement. A thread that reads a slightly outdated loss rate will still
  produce a valid (if suboptimal) scenario ordering.
- There are no dependent loads or stores that require ordering guarantees.
- The atomic increments themselves are correct (no lost updates) regardless of
  memory ordering.

### Lifetime and Scope

One `KillerDraws` instance is heap-allocated per `peg_solve` call and shared
across all phases:

1. **Greedy Phase 1a/1b** — passed via `PegGreedyThreadArgs`
2. **Greedy Phase 2** (sequential non-emptying candidates) — passed directly
3. **Greedy spread re-eval** — passed directly
4. **Endgame stages 1..N** — passed via `PegEndgameThreadArgs`

The same instance accumulates statistics across all phases, so later phases
benefit from earlier phases' observations. For example, the greedy evaluation
of 200+ candidates builds up a rich loss profile that the subsequent endgame
stages can exploit immediately.

Recursive inner `peg_solve` calls (from pass evaluation) allocate their own
independent `KillerDraws`. This is correct: the inner solve operates on a
different board position with different candidates, so the outer solver's loss
patterns don't transfer.

### Where Applied

| Code path | Draw type | Sorting | Update |
|---|---|---|---|
| `peg_endgame_eval_recursive_candidate` | `RecUpair` (unordered) | Bubble sort by `sort_key` desc, `total_weight` tiebreak | After all threads merge results |
| `peg_endgame_eval_candidate` (2-bag) | `PegPairScenario` (ordered) | `qsort` by `sort_key` desc, `weight` tiebreak | Inside iteration loop, per scenario |
| `peg_endgame_eval_candidate` (1-bag) | `PegSingleScenario` | `qsort` by `sort_key` desc, `count` tiebreak | Inside iteration loop, per scenario |
| Decomposed 1-bag endgame work queue | `eg_scenario_t1[]` arrays | Pre-sorted via `PegSingleScenario` before dispatch | Inside thread loop, per work item |

Not applied to `peg_greedy_eval_play` (the inline nested-loop greedy path for
bag-emptying candidates). Those per-scenario evaluations are fast (greedy
playout or 1-ply search), so the marginal benefit of reordering doesn't justify
refactoring the inline loops into a sorted array.

## Alternatives Considered

**Continuous loss weighting.** Instead of binary loss/not-loss, weight each
update by the magnitude of loss (e.g., `1.0 - win_rate`). Rejected because it
adds floating-point atomics complexity and the binary signal is sufficient for
ordering purposes.

**Per-stage killer tables.** Reset the table between stages so that deeper
endgame results don't mix with shallow greedy results. Rejected because
empirically the greedy phase's loss patterns correlate well with endgame
results (the same draws tend to be bad regardless of evaluation depth), and
accumulating across phases gives more data for the endgame stages.

**Decay or aging.** Reduce the weight of older observations to adapt to
changing cutoff thresholds. Rejected as unnecessary complexity — the table
grows monotonically but the loss *rate* naturally adjusts as new data arrives.

**Passing parent killer draws to recursive inner solves.** The inner
`peg_solve` (pass evaluation) could inherit the parent's table. Rejected
because the inner solve has a fundamentally different position (opponent is
on turn, different rack, different candidates). The parent's draw-level loss
patterns don't transfer meaningfully.
