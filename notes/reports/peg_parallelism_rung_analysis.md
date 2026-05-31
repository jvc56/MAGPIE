# PEG parallelism: do rung 4 (nested-cand) or rung 5 (MT endgames) matter?

**Date:** 2026-05-31
**Question:** The pessimistic PEG solver keeps ~17/18 cores busy mid-run but was
observed to thin out in places. Two unbuilt parallelism layers were candidates:

- **Rung 4** — parallelize `nested_solve`'s candidate loop. Unbuilt because the
  loop carries the cross-cand `minLossesSoFar` leader + early-WIN cutoff, so
  parallelizing it is pruning-delicate.
- **Rung 5** — multithread the leaf endgame solves themselves (now have the
  primitives: arbitrary MoveGen slots, `endgame_add_worker`, injectable inline
  solve, `peg_pool_idle_workers`).

To decide, two probes were added (off by default, reporting-only, verdict-neutral):

- `PASSPEG_PESSFULL_IDLE_PROBE_S` — when a leaf `endgame_solve` exceeds the
  threshold, sample `peg_pool_idle_workers()` (rung-5 signal).
- `PASSPEG_PESSFULL_RUNG4_PROBE_S` — when a `nested_solve` candidate loop exceeds
  the threshold, record whether it ran with no parallelism at this level
  (`parallel_perms == false`) and the idle-core count (rung-4 signal).

## Measurement: full uncapped P(AH), 2-ply

Config: `13M P..`, plies=2, 18 threads, 4 GB shared TT, SPLIT_OPP +
RECURSIVE_SPLIT, `max_opp_k=0` (uncapped), both probe thresholds = 10 ms.
First fully-completed uncapped run (prior runs were killed mid-flight).

```
W/L/D = 195/491/34  (29.44%)   <- matches validated deterministic value (probes are correctness-neutral)
solve_wall = 6605.80s (~110 min, includes probe overhead)   <- sustained ~17 cores throughout, no tail collapse
solves = 695,159    leaf_visits = 3.17M    nested_calls = 7,556    recursive = 3.19M

rung5-probe (leaf solves   >= 10ms): slow=694,377,  with>=2 idle cores=22.4%, avg idle=2.23
rung4-probe (nested cand loops>= 10ms): slow=552,   with>=2 idle cores=17.0%, avg idle=1.14, avg cands=2680
```

## Conclusion: for the uncapped pessimistic PAH, neither rung is worth building

- The coarse opp-split fan-out (170,937 work units across 358 orderings) keeps
  ~89% of cores fed for essentially the whole run. Avg idle during a leaf solve
  is 2.23 of 18 cores; during a heavy cand loop, 1.14.
- **Rung 5:** leaf solves dominate the work but coincide with >=2 idle cores only
  22% of the time. Marginal.
- **Rung 4:** only 552 heavy sequential cand loops in 110 min, and they mostly
  run while cores are busy (17% with >=2 idle, avg 1.14). Rare and low-idle —
  not worth the pruning-delicate build for this workload.
- The earlier-observed "tail collapse to ~8 cores" did **not** recur here; it was
  substantially run-to-run scheduling variance, not structural.

## The decisive reconciliation: narrow fan-out is what creates idle cores

A capped smoke test (`max_opp_k=2`) had shown scary rung-4 numbers (43% with >=2
idle, avg 4.4). The uncapped run flips that (17%, avg 1.1). The reason:

> **Capping opp moves shrinks the fan-out, and shrinking the fan-out is exactly
> what creates idle cores.** The uncapped solver is the *widest* possible
> workload, so it is the *least* idle.

Idle cores appear when the branching is **narrow**, not wide.

## Implication for the flagship cascade (`peg.h` -> future `peg.c`)

The pessimistic mode is the wide case; the cascade's deep stages are the narrow
case. The halving table (`32 -> 16 -> 8 -> 4 -> 2`) deliberately drives the
candidate count down while deepening the endgame toward 7-ply, so the deepest,
most expensive solves run at the fewest parallel units — structurally like the
*capped* case, where idle cores are real.

Therefore:

- **Do not** wire rung 4 or rung 5 into the pessimistic path. Measured, not worth
  it; the fan-out already saturates cores.
- **Rung 5 (MT endgames) is the right lever for `peg.c`'s deep stages**, where
  the narrow fan-out leaves cores idle on expensive deep solves. The primitives
  (arbitrary/non-contiguous MoveGen slots, `endgame_add_worker` for both spawned
  and inline solves, `peg_pool_idle_workers`) are built and TSAN-clean, ready to
  design in there rather than retrofit.
- Confirm once `peg.c` exists by pointing the same idle-probe at its deep stages.

See also: cascade stage granularity (a stage needs >=2 candidates to be usable;
no top-1 stage; top-2 are a parallel pair; skip a stage the budget can't fit).
