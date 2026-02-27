#ifndef ENDGAME_TIME_H
#define ENDGAME_TIME_H

// Per-turn time limits computed from a player's remaining budget.
typedef struct {
  double turn_limit; // wall-clock timer limit for this turn
  double soft_limit; // IDS soft limit (0 if time_mode == 0)
  double hard_limit; // IDS hard limit (0 if time_mode == 0)
} EndgameTurnLimits;

// Compute per-turn time limits from the remaining budget.
//
// budget_remaining:  seconds left in this player's budget
// player_turn_count: turns this player has already taken (for halving floor)
// tiles_on_rack:     current rack size (upper bound on future turns)
// time_mode:         0 = baseline (80% hard limit, no soft limit)
//                    1 = flexible (EBF with 60% soft / 90% hard + 50ms slack)
EndgameTurnLimits endgame_compute_turn_limits(double budget_remaining,
                                              int player_turn_count,
                                              int tiles_on_rack,
                                              int time_mode);

#endif
