#ifndef GAME_TIMER_H
#define GAME_TIMER_H

#include "../compat/ctime.h"
#include <math.h>
#include <stdbool.h>

// Per-side game clock. Each player's clock counts down while it is their
// turn. A player's nonpositive time control means that player is untimed.
// Engine components (for example the play chooser) can budget per-move
// thinking time from the remaining clock.
// Not thread safe.
typedef struct GameTimer {
  double time_per_player_seconds[2];
  double seconds_used[2];
  // Player whose clock is currently ticking, or -1 when no turn is active.
  int ticking_player_index;
  Timer turn_timer;
} GameTimer;

static inline void game_timer_reset_for_players(GameTimer *game_timer,
                                                double player0_seconds,
                                                double player1_seconds) {
  game_timer->time_per_player_seconds[0] = player0_seconds;
  game_timer->time_per_player_seconds[1] = player1_seconds;
  game_timer->seconds_used[0] = 0.0;
  game_timer->seconds_used[1] = 0.0;
  game_timer->ticking_player_index = -1;
  ctimer_reset(&game_timer->turn_timer);
}

static inline void game_timer_reset(GameTimer *game_timer,
                                    double time_per_side_seconds) {
  game_timer_reset_for_players(game_timer, time_per_side_seconds,
                               time_per_side_seconds);
}

static inline bool game_timer_player_is_untimed(const GameTimer *game_timer,
                                                int player_index) {
  return game_timer->time_per_player_seconds[player_index] <= 0.0;
}

static inline bool game_timer_is_untimed(const GameTimer *game_timer) {
  return game_timer_player_is_untimed(game_timer, 0) &&
         game_timer_player_is_untimed(game_timer, 1);
}

// Stops the currently ticking turn (if any), accumulating the elapsed time
// into the ticking player's used total.
static inline void game_timer_end_turn(GameTimer *game_timer) {
  if (game_timer->ticking_player_index < 0) {
    return;
  }
  ctimer_stop(&game_timer->turn_timer);
  game_timer->seconds_used[game_timer->ticking_player_index] +=
      ctimer_elapsed_seconds(&game_timer->turn_timer);
  game_timer->ticking_player_index = -1;
}

// Starts player_index's clock, ending any turn already in progress.
static inline void game_timer_start_turn(GameTimer *game_timer,
                                         int player_index) {
  game_timer_end_turn(game_timer);
  game_timer->ticking_player_index = player_index;
  ctimer_start(&game_timer->turn_timer);
}

static inline double game_timer_get_seconds_used(const GameTimer *game_timer,
                                                 int player_index) {
  double seconds_used = game_timer->seconds_used[player_index];
  if (game_timer->ticking_player_index == player_index) {
    seconds_used += ctimer_elapsed_seconds(&game_timer->turn_timer);
  }
  return seconds_used;
}

// Seconds left on player_index's clock, clamped at zero. Untimed games
// return INFINITY.
static inline double
game_timer_get_seconds_remaining(const GameTimer *game_timer,
                                 int player_index) {
  if (game_timer_player_is_untimed(game_timer, player_index)) {
    return INFINITY;
  }
  const double seconds_remaining =
      game_timer->time_per_player_seconds[player_index] -
      game_timer_get_seconds_used(game_timer, player_index);
  return seconds_remaining > 0.0 ? seconds_remaining : 0.0;
}

static inline bool game_timer_is_expired(const GameTimer *game_timer,
                                         int player_index) {
  return !game_timer_player_is_untimed(game_timer, player_index) &&
         game_timer_get_seconds_remaining(game_timer, player_index) <= 0.0;
}

static inline double
game_timer_get_overtime_seconds(const GameTimer *game_timer, int player_index) {
  if (game_timer_player_is_untimed(game_timer, player_index)) {
    return 0.0;
  }
  const double overtime =
      game_timer_get_seconds_used(game_timer, player_index) -
      game_timer->time_per_player_seconds[player_index];
  return overtime > 0.0 ? overtime : 0.0;
}

#endif
