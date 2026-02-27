#include "endgame_time.h"

EndgameTurnLimits endgame_compute_turn_limits(double budget_remaining,
                                              int player_turn_count,
                                              int tiles_on_rack,
                                              int time_mode) {
  // Subtract 50ms from internal soft/hard limits to leave headroom for
  // the ~50ms timer-thread join that gets charged to wall clock.
  const double slack = 0.05;

  // Halving floor: 500ms on first turn, halves each player turn, minimum 20ms.
  // Ensures a minimum search even when budget is low.
  const double floor_min = 0.020;
  double floor_secs = 0.500 / (double)(1 << player_turn_count);
  if (floor_secs < floor_min) {
    floor_secs = floor_min;
  }

  // Reserve floor_min for every remaining turn after this one so future turns
  // are never starved below the floor.
  // Upper bound: each turn plays at least one tile, so at most
  // (tiles_on_rack - 1) turns remain after this one.
  int turns_left_after = tiles_on_rack > 1 ? tiles_on_rack - 1 : 0;
  double reserved = (double)turns_left_after * floor_min;
  double spendable = budget_remaining - reserved;
  if (spendable < floor_secs) {
    spendable = floor_secs;
  }

  EndgameTurnLimits limits = {0};
  if (time_mode == 0) {
    // Baseline: 80% of spendable budget as hard limit
    limits.turn_limit = spendable * 0.80;
  } else {
    // Flexible: EBF-based time management inside the solver.
    // soft = 60% of spendable, hard = 90% of spendable (both minus slack).
    limits.soft_limit = spendable * 0.60 - slack;
    limits.hard_limit = spendable * 0.90 - slack;
    if (limits.soft_limit < 0) {
      limits.soft_limit = 0;
    }
    if (limits.hard_limit < 0) {
      limits.hard_limit = 0;
    }
    limits.turn_limit = spendable * 0.90; // external timer: no slack
  }

  // Apply floor
  if (limits.turn_limit < floor_secs) {
    limits.turn_limit = floor_secs;
  }

  return limits;
}
