#ifndef ENDGAME_H
#define ENDGAME_H

#include <stdbool.h>

#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/kwg.h"
#include "../ent/move.h"
#include "../ent/small_move_arena.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"

enum {
  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE = 1024 * 1024,
};

typedef struct EndgameSolver EndgameSolver;

// Callback for per-ply PV reporting during iterative deepening
// Parameters: depth, value (spread delta), pv_line, game, user_data
typedef void (*EndgamePerPlyCallback)(int depth, int32_t value,
                                      const struct PVLine *pv_line,
                                      const struct Game *game, void *user_data);

typedef struct EndgameArgs {
  ThreadControl *thread_control;
  const Game *game;
  double tt_fraction_of_mem;
  int plies;
  int initial_small_move_arena_size;
  EndgamePerPlyCallback per_ply_callback;
  void *per_ply_callback_data;
  bool use_wordprune;
} EndgameArgs;

EndgameSolver *endgame_solver_create(void);
void endgame_solve(EndgameSolver *solver, const EndgameArgs *endgame_args,
                   EndgameResults *results, ErrorStack *error_stack);
void endgame_solver_destroy(EndgameSolver *es);

void string_builder_endgame_results(StringBuilder *pv_description,
                                    const EndgameResults *results,
                                    const Game *game,
                                    const GameHistory *game_history,
                                    bool add_line_breaks);
char *endgame_results_get_string(const EndgameResults *results,
                                 const Game *game,
                                 const GameHistory *game_history,
                                 bool add_line_breaks);
#endif
