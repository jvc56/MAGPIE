#ifndef ENDGAME_H
#define ENDGAME_H

#include "../ent/arena.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"

typedef struct PVLine {
  Move **moves;
  Game *game;
  int32_t score;
  int num_moves;
} PVLine;

typedef struct EndgameSolver {

  Game **game_copies;

  int initial_spread;
  int solving_player;
  SmallMove *initial_moves;
  int n_initial_moves;

  bool iterative_deepening_optim;
  bool first_win_optim;
  bool transposition_table_optim;
  bool negascout_optim;
  bool lazysmp_optim;
  bool prevent_slowroll;
  PVLine principal_variation;
  PVLine *variations;

  int solve_multiple_variations;
  int32_t best_pv_value;
  int requested_plies;
  int threads;

  // Owned by the caller:
  ThreadControl *thread_control;

} EndgameSolver;

typedef struct EndgameSolverWorker {
  int thread_index;
  Game *game;
  Arena *small_move_arena;
  MoveList *move_list;
  EndgameSolver *solver;
  int current_iterative_deepening_depth;
  int current_arena_pointer;
} EndgameSolverWorker;

#endif
