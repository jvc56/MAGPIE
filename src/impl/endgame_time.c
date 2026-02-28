#include "endgame_time.h"

EndgameTurnLimits endgame_compute_turn_limits(double budget_remaining,
                                              int player_turn_count,
                                              int tiles_on_rack,
                                              int time_mode) {
  const double slack = ET_SLACK_MS / 1000.0;
  const double floor_min = ET_FLOOR_MIN_MS / 1000.0;
  const double floor_start = ET_FLOOR_START_MS / 1000.0;
  const double response_margin = ET_RESPONSE_MARGIN_MS / 1000.0;
  const double min_turn_limit = ET_MIN_TURN_LIMIT_MS / 1000.0;
  const double timer_early = ET_TIMER_EARLY_MS / 1000.0;
  const double floor_budget_cap = ET_FLOOR_BUDGET_CAP_PCT / 100.0;
  const double baseline_limit = ET_BASELINE_LIMIT_PCT / 100.0;
  const double soft_frac = ET_SOFT_LIMIT_PCT / 100.0;
  const double hard_frac = ET_HARD_LIMIT_PCT / 100.0;

  // Halving floor: floor_start on first turn, halves each player turn,
  // minimum floor_min.  Ensures a minimum search even when budget is low.
  // Cap at floor_budget_cap of remaining budget so bullet/tiny budgets
  // don't overshoot on the very first turn.
  int shift = player_turn_count < 31 ? player_turn_count : 31;
  double floor_secs = floor_start / (double)(1u << shift);
  if (floor_secs > budget_remaining * floor_budget_cap) {
    floor_secs = budget_remaining * floor_budget_cap;
  }
  if (floor_secs < floor_min) {
    floor_secs = floor_min;
  }

  // Reserve floor_min only for future turns that are expected to run the
  // solver (turn_limit ≥ timer_early).  Each solver turn consumes at least
  // timer_early seconds, so the expected count is:
  //   solver_turns = floor((budget − turns_left × floor_min) / timer_early)
  // clamped to [0, turns_left_after].
  //
  // For large budgets (standard time controls) this equals the full
  // turns_left reservation, preserving the original behaviour.  For bullet
  // budgets it is reduced so the current turn gets more of the remaining
  // budget rather than hoarding it for future static-eval turns.
  int turns_left_after = tiles_on_rack > 1 ? tiles_on_rack - 1 : 0;
  int solver_turns_remaining =
      (int)((budget_remaining - turns_left_after * floor_min) / timer_early);
  if (solver_turns_remaining > turns_left_after) {
    solver_turns_remaining = turns_left_after;
  }
  if (solver_turns_remaining < 0) {
    solver_turns_remaining = 0;
  }
  double reserved = (double)solver_turns_remaining * floor_min;
  double spendable = budget_remaining - reserved;
  if (spendable < floor_secs) {
    spendable = floor_secs;
  }

  EndgameTurnLimits limits = {0};
  if (time_mode == 0) {
    // Baseline: baseline_limit fraction of spendable budget as hard limit.
    limits.turn_limit = spendable * baseline_limit;
  } else {
    // Flexible: EBF-based time management inside the solver.
    // soft = soft_frac of spendable, hard = hard_frac of spendable
    // (both minus slack).
    limits.soft_limit = spendable * soft_frac - slack;
    limits.hard_limit = spendable * hard_frac - slack;
    if (limits.soft_limit < 0) {
      limits.soft_limit = 0;
    }
    if (limits.hard_limit < 0) {
      limits.hard_limit = 0;
    }
    limits.turn_limit =
        spendable * hard_frac - slack; // external timer: same slack
  }

  // Apply floor.
  if (limits.turn_limit < floor_secs) {
    limits.turn_limit = floor_secs;
  }

  // Safety cap: ensure turn_limit + response_margin does not exceed
  // budget_remaining.  When budget is nearly exhausted the floor can push
  // turn_limit high enough that the solver's startup + response latency burns
  // past the remaining clock.  This is separate from the EBF slack above.
  if (limits.turn_limit + response_margin > budget_remaining) {
    limits.turn_limit = budget_remaining - response_margin;
    if (limits.turn_limit < min_turn_limit) {
      limits.turn_limit = min_turn_limit;
    }
  }

  // Compute the actual external timer value.  Fire the timer timer_early
  // seconds before turn_limit so that the solver has enough time to reach
  // pruned search depths before interrupt, keeping response latency well
  // below timer_early.  elapsed ≈ timer_secs + response_latency ≈ turn_limit.
  limits.timer_secs = limits.turn_limit - timer_early;
  if (limits.timer_secs < min_turn_limit) {
    limits.timer_secs = min_turn_limit;
  }

  // Skip the solver when turn_limit < timer_early: no useful pruned search
  // window — the solver cannot complete even one full IDS iteration before
  // the interrupt fires, leaving response latency ≈ full movegen time which
  // can exceed turn_limit.  Use fast static evaluation instead.
  limits.use_static_eval = (limits.turn_limit < timer_early);

  // Use 1 thread for non-static-eval turns where remaining budget is small.
  // 8-thread join overhead (~8 ms) dominates search quality when budget is low.
  limits.use_single_thread =
      (!limits.use_static_eval &&
       budget_remaining < ET_SINGLE_THREAD_BUDGET_MS / 1000.0);

  return limits;
}
