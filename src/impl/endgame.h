#ifndef ENDGAME_H
#define ENDGAME_H

#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/move.h"
#include "../ent/small_move_arena.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"

// We don't expect an endgame length to ever be larger than this value.
#define MAX_VARIANT_LENGTH 25

typedef struct PVLine {
  SmallMove moves[MAX_VARIANT_LENGTH];
  Game *game;
  int32_t score;
  int num_moves;
} PVLine;

typedef struct EndgameSolver {
  int initial_spread;
  int solving_player;
  int n_initial_moves;

  int initial_small_move_arena_size;
  bool iterative_deepening_optim;
  bool first_win_optim;
  bool transposition_table_optim;
  bool negascout_optim;
  bool lazysmp_optim;
  bool prevent_slowroll;
  bool wordprune_optim;
  PVLine principal_variation;
  PVLine *variations;

  KWG *pruned_kwg;
  int nodes_searched;

  int solve_multiple_variations;
  int32_t best_pv_value;
  int requested_plies;
  int threads;
  TranspositionTable *transposition_table;

  // Owned by the caller:
  ThreadControl *thread_control;
  const Game *game;

} EndgameSolver;

typedef struct EndgameSolverWorker {
  int thread_index;
  Game *game_copy;
  Arena *small_move_arena;
  MoveList *move_list;
  EndgameSolver *solver;
  int current_iterative_deepening_depth;
} EndgameSolverWorker;

EndgameSolver *endgame_solver_create(ThreadControl *tc, const Game *game,
                                     double tt_fraction_of_mem);
PVLine endgame_solve(EndgameSolver *solver, int plies);
void endgame_solver_destroy(EndgameSolver *es);

#endif
