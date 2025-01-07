#include "endgame.h"
#include "move_gen.h"

#define DEFAULT_ENDGAME_MOVELIST_CAPACITY 250000

// Bit flags for move estimates. These large numbers will force these estimated
// values to sort first.
#define EARLY_PASS_BF 1 << 29
#define HASH_MOVE_BF 1 << 28
#define GOING_OUT_BF 1 << 27

#define LARGE_VALUE 1 << 30 // for ab

EndgameSolver *endgame_solver_create(ThreadControl *tc, Game *game) {
  EndgameSolver *es = malloc_or_die(sizeof(EndgameSolver));
  es->first_win_optim = false;
  es->transposition_table_optim = false; // for now
  es->iterative_deepening_optim = true;
  es->negascout_optim = true;
  es->solve_multiple_variations = 0;
  es->threads = 1; // for now
  es->solving_player = game_get_player_on_turn_index(game);
  Player *player = game_get_player(game, es->solving_player);
  Player *opponent = game_get_player(game, 1 - es->solving_player);

  es->initial_spread = player_get_score(player) - player_get_score(opponent);
  es->threads = thread_control_get_threads(tc);
  es->thread_control = tc;

  return es;
}

EndgameSolverWorker *endgame_solver_create_worker(const Game *game,
                                                  EndgameSolver *solver,
                                                  int worker_index) {

  EndgameSolverWorker *solver_worker =
      malloc_or_die(sizeof(EndgameSolverWorker));

  solver_worker->thread_index = worker_index;
  solver_worker->game = game_duplicate(game);
  game_set_endgame_solving_mode(solver_worker->game);
  game_set_backup_mode(solver_worker->game, BACKUP_MODE_SIMULATION);
  solver_worker->move_list =
      move_list_create_small(DEFAULT_ENDGAME_MOVELIST_CAPACITY);

  solver_worker->small_move_arena = create_arena(0, 16);

  solver_worker->solver = solver;

  return solver_worker;
}

void solver_worker_destroy(EndgameSolverWorker *solver_worker) {
  if (!solver_worker) {
    return;
  }
  game_destroy(solver_worker->game);
  small_move_list_destroy(solver_worker->move_list);
  arena_destroy(solver_worker->small_move_arena);
  free(solver_worker);
}

int generate_stm_plays(EndgameSolverWorker *worker, int depth) {
  // stm means side to move
  // This won't actually sort by score. We'll do this later.
  generate_moves(worker->game, MOVE_RECORD_ALL_SMALL, MOVE_SORT_SCORE,
                 worker->thread_index, worker->move_list);
  SmallMove *arena_small_moves = (SmallMove *)arena_alloc(
      worker->small_move_arena, worker->move_list->count * sizeof(SmallMove));
  for (int i = 0; i < worker->move_list->count; i++) {
    // Copy by value
    arena_small_moves[i] = *(worker->move_list->small_moves[i]);
  }
  // Now that the arena has these, we can deal with the move list directly
  // in the arena.
  return worker->move_list->count;
}

int assign_estimates(EndgameSolverWorker *worker, int depth, int arena_begin,
                     int move_count) {
  // assign estimates to arena plays.

  Player *player =
      game_get_player(worker->game, worker->solver->solving_player);
  Player *opponent =
      game_get_player(worker->game, 1 - worker->solver->solving_player);
  Rack *stm_rack = player_get_rack(player);
  Rack *other_rack = player_get_rack(opponent);
  int ntiles_on_rack = stm_rack->number_of_letters;
  bool last_move_was_pass = game_get_consecutive_scoreless_turns(
                                worker->game) == 1; // check if this is ok.

  SmallMove *small_moves =
      (SmallMove *)(worker->small_move_arena->memory + arena_begin);

  for (size_t i = 0; i < move_count; i++) {
    SmallMove *current_move = &small_moves[i];
    if (small_move_get_tiles_played(current_move) == ntiles_on_rack) {
      small_move_set_estimated_value(
          current_move,
          small_move_get_score(current_move) +
              2 * rack_get_score(game_get_ld(worker->game), other_rack) +
              GOING_OUT_BF);
    } else if (depth > 2) {
      // some more jitter for lazysmp
      if (worker->thread_index >= 6) {
        small_move_set_estimated_value(
            current_move, small_move_get_score(current_move) +
                              3 * small_move_get_tiles_played(current_move));
      } else {
        small_move_set_estimated_value(
            current_move, small_move_get_score(current_move) -
                              5 * small_move_get_tiles_played(current_move));
      }
    } else {
      small_move_set_estimated_value(current_move,
                                     small_move_get_score(current_move));
    }

    // TODO: add ttMove once available

    // Consider pass first if we've just passed. This will allow us to see that
    // branch of the tree faster.
    if (last_move_was_pass && small_move_is_pass(current_move)) {
      small_move_add_estimated_value(current_move, EARLY_PASS_BF);
    }
  }
  // sort moves by estimated value, from biggest to smallest value. A good move
  // sorting is instrumental to the performance of ab pruning.
  qsort(small_moves, move_count, sizeof(SmallMove),
        compare_small_moves_by_estimated_value);
}

void iterative_deepening(EndgameSolverWorker *worker, int plies) {

  int32_t alpha = -LARGE_VALUE;
  int32_t beta = LARGE_VALUE;

  if (worker->solver->first_win_optim) {
    // search a very small window centered around 0; we're just trying to find
    // something that surpasses it.
    alpha = -1;
    beta = 1;
  }
  assert(worker->small_move_arena->size == 0); // make sure arena is empty.
  int initial_move_count = generate_stm_plays(worker, 0);
  // Arena pointer better have started at 0, since it was empty.
  assign_estimates(worker, 0, 0, initial_move_count);
  SmallMove *initial_moves = (SmallMove *)(worker->small_move_arena->memory);

  SmallMove last_winner;

  worker->current_iterative_deepening_depth = 1;
  int start = 1;
  if (!worker->solver->iterative_deepening_optim) {
    start = plies;
  }

  for (int p = start; p <= plies; p++) {
    worker->current_iterative_deepening_depth = p;
    PVLine pv;
    pv.game = worker->game;
    int32_t val = negamax(worker, p, alpha, beta, &pv, true);
    // sort moves by valuation for next time.
    qsort(initial_moves, initial_move_count, sizeof(SmallMove),
          compare_small_moves_by_estimated_value);
    worker->solver->best_pv_value = val - worker->solver->initial_spread;
    // TODO: assign PV
  }
}

int32_t negamax(EndgameSolverWorker *worker, int depth, int32_t alpha,
                int32_t beta, PVLine *pv, bool pvNode) {}