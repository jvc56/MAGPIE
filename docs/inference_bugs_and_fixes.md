# Inference Bugs Discovered and Fixed

## Bug 1: min_play_iterations=1 Causes BAI to Permanently Write Off Bingos

### Symptom

INFER1 played VILLI (16 pts) instead of the bingo VITELLIN (64 pts) on
turn 2 of seed 7014. STATIC, facing the same position, played VITELLIN.

### Root Cause

With `min_play_iterations=1` (aka `sample_minimum=1`), BAI (Best Arm
Identification) gives each move arm exactly 1 initial playout sample.
If that single playout happens to lose, BAI permanently deprioritizes the
move and never revisits it. For bingos, which score well but represent only
one of many move options, a single unlucky playout can write off a 64-point
play forever.

### Diagnosis

Created `test/debug_infer_turn2.c` to replay seed 7014 turn 2 under three
configurations:

| Configuration | Best Move | Wp | Iterations |
|---|---|---|---|
| SIM with inference | VITELLIN 64 | 53.2% | 97,993 |
| SIM without inference, minp=1 | VILLI 16 | 37.7% | (VITELLIN got 1 iter, 26.3% Wp) |
| SIM without inference, minp=100 | VITELLIN 64 | 53.1% | 48,611 |

With minp=1, VITELLIN received exactly 1 sample, got an unlucky 26.3% win
probability estimate, and was never sampled again. With minp=100, BAI gave
it enough samples to converge to its true ~53% win probability.

### Fix

Changed `min_play_iterations` from 1 to 50 in `roundrobin_benchmark.c`:

```c
sim_args_fill(..., 50, ...);  // was 1
```

50 was chosen as a balance: enough to avoid single-sample write-offs, but
not so many that the initial phase consumes the entire simulation budget.

### Lesson

BAI's initial sampling phase is critical. With too few initial samples per
arm, the algorithm degenerates into a lottery where high-variance moves
(like bingos) get permanently discarded based on noise.

---

## Bug 2: Stale sim_results After Budget Starvation

### Symptom

Tournament 3 crashed at seed 8019 with:
```
FATAL src/impl/inference.c:306: failed to draw nontarget player rack
(ACEFGJKMOOO...)
```
The rack contained hundreds of O's — clearly corrupted memory.

### Root Cause

A chain of failures:

1. **Inference eats the budget:** With a 2s turn budget and 1s inference
   limit, inference sometimes takes >1s (due to timer granularity), leaving
   <1s for simulation.

2. **Simulation runs with insufficient time:** With minp=50 and ~15 move
   arms, the initial BAI phase needs 750 samples. With only ~0.84s remaining,
   the timer fires during the initial phase.

3. **sim_results retains stale data:** `SimResults` is never fully reset
   between `simulate()` calls. When BAI stops early (during initial phase),
   the results contain iteration counts and win percentages from the
   *previous* turn's simulation.

4. **Stale results cause duplicate move:** The benchmark reads the "best
   move" from sim_results, but it's actually the best move from the
   previous turn. Playing the same move twice corrupts the game state.

5. **Corruption cascades:** The next inference call reads the corrupted game
   state, tries to draw tiles from a bag that doesn't make sense, and hits
   the fatal assertion.

### Fix

Added a budget-too-low fallback after inference. If the remaining simulation
budget is less than 1.0s, skip simulation entirely and fall back to the
equity-best move:

```c
double sim_budget = turn_budget - infer_elapsed;
if (sim_budget < 1.0) {
    printf("*** Budget too low (%.2fs), falling back to equity-best ***\n",
           sim_budget);
    generate_moves(&gen_args_fb);
    move = move_list_get_move(move_list, 0);
    play_move(move, game, NULL);
    goto turn_done;
}
```

### Underlying Issue (Not Fully Fixed)

The root cause — `SimResults` not being reset between calls — remains.
The budget fallback prevents the crash but doesn't fix the data staleness.
A more robust fix would be to always reset `SimResults` at the start of
`simulate()`, or to have `simulate()` return an error/flag when BAI
doesn't complete its initial phase.

### Lesson

Shared mutable state (`SimResults`) that persists across calls is dangerous.
When callers assume results reflect the current call but the implementation
can short-circuit and leave old data in place, subtle corruption follows.

---

## Bug 3: Error Check After simulate() Doesn't Catch All Failures

### Symptom

An earlier fix added an error check after `simulate()`:

```c
if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    // fall back to equity-best
}
```

This caught some sim failures but NOT the budget starvation case, because
`simulate()` doesn't push an error when BAI stops early due to timer
interrupt. The timer-based interruption is a normal exit path, not an error.

### Lesson

Error-based fallback only works if the callee reports errors for all failure
modes. Silent early exits (like timer interrupts) need separate detection —
either by checking iteration counts or by having the caller verify that
results are fresh.

---

## Bug 4: inference_results Not Set on sim Reset

### File

`src/impl/random_variable.c`, line 578

### Fix

Added:
```c
simmer->inference_results = sim_args->inference_results;
```

Without this, when `rv_sim_reset()` is called to prepare for a new
simulation run, the simmer's inference_results pointer was stale,
pointing to results from a previous run or uninitialized memory.
