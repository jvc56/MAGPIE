#ifndef ENDGAME_H
#define ENDGAME_H

#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/move.h"
#include "../ent/small_move_arena.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"

enum {
  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE = 1024 * 1024,
};

typedef struct EndgameSolver EndgameSolver;

typedef struct EndgameArgs {
  ThreadControl *thread_control;
  const Game *game;
  double tt_fraction_of_mem;
  int plies;
  int initial_small_move_arena_size;
} EndgameArgs;

EndgameSolver *endgame_solver_create(void);
void endgame_solve(EndgameSolver *solver, const EndgameArgs *endgame_args,
                   EndgameResults *results, ErrorStack *error_stack);
void endgame_solver_destroy(EndgameSolver *es);

void string_builder_add_pvline(const PVLine *pv_line, const Game *game,
                               bool add_line_breaks,
                               StringBuilder *pv_description);
#endif
