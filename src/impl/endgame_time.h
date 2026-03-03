#ifndef ENDGAME_TIME_H
#define ENDGAME_TIME_H

#include <stdbool.h>

// Timing constants for endgame_compute_turn_limits.
// _MS values are milliseconds; _PCT values are integer percentages (0–100).
typedef enum {
  // Subtracted from EBF soft/hard limits: headroom for timer poll (~5 ms)
  // plus average solver response latency (~15 ms) after interrupt fires.
  ET_SLACK_MS = 20,

  // Minimum per-turn search time; the floor never drops below this value.
  ET_FLOOR_MIN_MS = 20,

  // First-turn floor before halving each player turn:
  // 200 ms → 100 ms → 50 ms → 25 ms → … → ET_FLOOR_MIN_MS.
  ET_FLOOR_START_MS = 200,

  // Safety-cap margin: turn_limit is clamped to (budget − this value).
  // Covers the irreducible solver response latency after USER_INTERRUPT fires:
  // generate_stm_plays completion (~10 ms) + 8× pthread_join (~8 ms).
  ET_RESPONSE_MARGIN_MS = 30,

  // Absolute minimum turn_limit after the safety-cap clamp (avoids 0).
  ET_MIN_TURN_LIMIT_MS = 1,

  // External timer fires this many ms before the nominal turn_limit so that
  // the solver's response latency lands within turn_limit.
  //
  // Response latency after USER_INTERRUPT is dominated by completing the
  // current generate_stm_plays() call.  At shallow IDS depths the solver
  // is generating moves for near-full-rack positions (100–200 ms); at
  // deeper depths alpha-beta pruning keeps each call fast (~20–90 ms).
  //
  // ET_TIMER_EARLY_MS = 100 ms: fires the external timer 100 ms before the
  // nominal turn_limit.  By 100 ms into the search, the solver is past the
  // most expensive shallow IDS iterations; subsequent generate_stm_plays()
  // calls complete within ~20–90 ms, so elapsed ≈ turn_limit after interrupt.
  // Turns where turn_limit < 100 ms have no useful pruned search window and
  // fall back to static evaluation.
  //
  // Also used to estimate how many future turns will run the solver vs. fall
  // back to static eval (see endgame_compute_turn_limits).
  ET_TIMER_EARLY_MS = 100,

  // Below this remaining budget, use 1 thread instead of the default count.
  // 8-thread join overhead (~8 ms) dominates when budget is small; 1 thread
  // costs ~1 ms.  Keep this below the minimum useful bullet budget so turns
  // with enough time still use 8-thread parallel search.
  ET_SINGLE_THREAD_BUDGET_MS = 200,

  // Cap the halving floor at this percentage of the remaining budget so
  // bullet/tiny budgets do not overshoot on the very first turn.
  ET_FLOOR_BUDGET_CAP_PCT = 50,

  // Baseline mode: hard limit as percentage of spendable budget.
  ET_BASELINE_LIMIT_PCT = 80,

  // Flexible mode: EBF soft limit as percentage of spendable budget.
  ET_SOFT_LIMIT_PCT = 60,

  // Flexible mode: EBF hard limit and external turn_limit as percentage
  // of spendable budget.
  ET_HARD_LIMIT_PCT = 90,
} EndgameTimingConst;

// Per-turn time limits computed from a player's remaining budget.
typedef struct {
  double turn_limit; // nominal per-turn budget (for display and delta tracking)
  double timer_secs; // actual external timer value = turn_limit −
                     // ET_TIMER_EARLY_MS (fire early so elapsed ≈ turn_limit
                     // after response latency)
  double soft_limit; // IDS soft limit (0 if time_mode == 0)
  double hard_limit; // IDS hard limit (0 if time_mode == 0)
  // When true, caller should use 1 thread instead of the default count.
  // Spin-up/join overhead (~8 ms) dominates when
  // budget < ET_SINGLE_THREAD_BUDGET_MS ms.
  bool use_single_thread;
  // When true, turn_limit < ET_TIMER_EARLY_MS — no useful pruned search window.
  // Caller should skip the solver and use a fast static evaluation instead.
  bool use_static_eval;
} EndgameTurnLimits;

// Compute per-turn time limits from the remaining budget.
//
// budget_remaining:  seconds left in this player's budget
// player_turn_count: turns this player has already taken (for halving floor)
// tiles_on_rack:     current rack size (upper bound on future turns)
// time_mode:         0 = baseline (ET_BASELINE_LIMIT_PCT% hard limit, no soft)
//                    1 = flexible (EBF: ET_SOFT_LIMIT_PCT% soft /
//                        ET_HARD_LIMIT_PCT% hard − ET_SLACK_MS ms slack)
//
// Reservation: only floor_min is reserved per future turn that is expected to
// run the solver (turn_limit ≥ ET_TIMER_EARLY_MS).  Since each solver turn
// costs at least ET_TIMER_EARLY_MS seconds, the number of expected solver turns
// is estimated as (budget − tiles×floor_min) / ET_TIMER_EARLY_MS.  Turns
// expected to fall back to static eval need no reservation.  For large budgets
// (standard time controls) this equals the full turns_left reservation;
// for bullet budgets it is reduced, front-loading budget to the current turn.
//
// Sets use_static_eval when turn_limit < ET_TIMER_EARLY_MS ms (no useful
// pruned search window), and use_single_thread when budget <
// ET_SINGLE_THREAD_BUDGET_MS ms (8-thread join overhead dominates).
// timer_secs = turn_limit − ET_TIMER_EARLY_MS; callers pass timer_secs to the
// external wall-clock timer so elapsed ≈ turn_limit after solver response.
EndgameTurnLimits endgame_compute_turn_limits(double budget_remaining,
                                              int player_turn_count,
                                              int tiles_on_rack, int time_mode);

#endif
