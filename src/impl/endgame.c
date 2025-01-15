#include <pthread.h>

#include "../../src/ent/move.h"
#include "../../src/str/move_string.h"
#include "../../src/util/string_util.h"
#include "endgame.h"
#include "gameplay.h"
#include "move_gen.h"

#define DEFAULT_ENDGAME_MOVELIST_CAPACITY 250000

// Bit flags for move estimates. These large numbers will force these estimated
// values to sort first.
#define EARLY_PASS_BF (1 << 29)
#define HASH_MOVE_BF (1 << 28)
#define GOING_OUT_BF (1 << 27)
#define MAX(a, b) ((a) > (b) ? (a) : (b))

const int32_t LARGE_VALUE = (1 << 30); // for alpha-beta pruning

void pvline_clear(PVLine *pv_line) {
  pv_line->num_moves = 0;
  pv_line->score = 0;
}

void pvline_update(PVLine *pv_line, const PVLine *new_pv_line, SmallMove *move,
                   int32_t score) {
  pvline_clear(pv_line);
  pv_line->moves[0].metadata = move->metadata;
  pv_line->moves[0].tiny_move = move->tiny_move;
  for (int i = 0; i < new_pv_line->num_moves; i++) {
    assert(i != MAX_VARIANT_LENGTH - 1);
    pv_line->moves[i + 1].metadata = new_pv_line->moves[i].metadata;
    pv_line->moves[i + 1].tiny_move = new_pv_line->moves[i].tiny_move;
  }
  pv_line->num_moves = new_pv_line->num_moves + 1;
  pv_line->score = score;
}

StringBuilder *pvline_string(const PVLine *pv_line, const Game *game,
                             bool add_line_breaks) {
  StringBuilder *pv_description = string_builder_create();
  Move *temp = move_create();
  string_builder_add_formatted_string(
      pv_description, "<PV (Value %d, seqlen %d)>%c", pv_line->score,
      pv_line->num_moves, add_line_breaks ? '\n' : ' ');
  for (int i = 0; i < pv_line->num_moves; i++) {
    string_builder_add_formatted_string(pv_description, "%d: ", i + 1);
    small_move_to_move(temp, &(pv_line->moves[i]), game_get_board(game));

    string_builder_add_move_description(pv_description, temp,
                                        game_get_ld(game));
    string_builder_add_formatted_string(pv_description, "%c",
                                        add_line_breaks ? '\n' : ' ');
  }
  move_destroy(temp);
  return pv_description;
}

EndgameSolver *endgame_solver_create(ThreadControl *tc, const Game *game) {
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
  // later, when we have multi-threaded endgame:
  // es->threads = thread_control_get_threads(tc);
  es->thread_control = tc;
  es->game = game;

  return es;
}

void endgame_solver_destroy(EndgameSolver *es) {
  if (!es) {
    return;
  }
  free(es);
}

EndgameSolverWorker *endgame_solver_create_worker(EndgameSolver *solver,
                                                  int worker_index) {

  EndgameSolverWorker *solver_worker =
      malloc_or_die(sizeof(EndgameSolverWorker));

  solver_worker->thread_index = worker_index;
  solver_worker->game_copy = game_duplicate(solver->game);
  game_set_endgame_solving_mode(solver_worker->game_copy);
  game_set_backup_mode(solver_worker->game_copy, BACKUP_MODE_SIMULATION);
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
  game_destroy(solver_worker->game_copy);
  small_move_list_destroy(solver_worker->move_list);
  arena_destroy(solver_worker->small_move_arena);
  free(solver_worker);
}

int generate_stm_plays(EndgameSolverWorker *worker) {
  // stm means side to move
  // This won't actually sort by score. We'll do this later.

  {
    // XXX: Remove this assert once we fix bugs
    int letters = rack_get_total_letters(player_get_rack(game_get_player(
        worker->game_copy, game_get_player_on_turn_index(worker->game_copy))));
    // log_warn("generating with %d letters", letters);
    assert(letters > 0);
  }

  generate_moves(worker->game_copy, MOVE_RECORD_ALL_SMALL, MOVE_SORT_SCORE,
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

void assign_estimates_and_sort(EndgameSolverWorker *worker, int depth,
                               int move_count) {
  // assign estimates to arena plays.
  // log_warn("assigning estimates; depth %d, arena_begin %d, move_count %d",
  //          depth, worker->small_move_arena->size, move_count);
  Player *player =
      game_get_player(worker->game_copy, worker->solver->solving_player);
  Player *opponent =
      game_get_player(worker->game_copy, 1 - worker->solver->solving_player);
  Rack *stm_rack = player_get_rack(player);
  Rack *other_rack = player_get_rack(opponent);
  int ntiles_on_rack = stm_rack->number_of_letters;
  bool last_move_was_pass = game_get_consecutive_scoreless_turns(
                                worker->game_copy) == 1; // check if this is ok.

  int arena_offset =
      worker->small_move_arena->size - (sizeof(SmallMove) * move_count);

  SmallMove *small_moves =
      (SmallMove *)(worker->small_move_arena->memory + arena_offset);

  for (size_t i = 0; i < (size_t)move_count; i++) {
    SmallMove *current_move = &small_moves[i];
    // log_warn("assign estimate to move idx %d, tm %x, meta %x", i,
    //          current_move->tiny_move, current_move->metadata);
    if (small_move_get_tiles_played(current_move) == ntiles_on_rack) {
      small_move_set_estimated_value(
          current_move,
          small_move_get_score(current_move) +
              (2 * rack_get_score(game_get_ld(worker->game_copy), other_rack)) +
              GOING_OUT_BF);
    } else if (depth > 2) {
      // some more jitter for lazysmp (to be implemented)
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

char *create_spaces(int depth) {
  // Allocate memory for the string of spaces (+1 for the null terminator)
  char *spaces = (char *)malloc(depth + 1);

  // Fill the string with spaces
  for (int i = 0; i < depth; i++) {
    spaces[i] = ' ';
  }

  // Null-terminate the string
  spaces[depth] = '\0';

  return spaces;
}

// void print_small_plays(EndgameSolverWorker *worker, int nplays,
//                        int cur_move_loc) {}

int32_t negamax(EndgameSolverWorker *worker, int depth, int32_t alpha,
                int32_t beta, PVLine *pv, bool pv_node) {

  assert(pv_node || alpha == beta - 1);
  // int32_t alpha_orig = alpha;

  if (depth == 0 ||
      game_get_game_end_reason(worker->game_copy) != GAME_END_REASON_NONE) {
    // This assumes the player turn changed even though the game was already
    // over, which appears to be the case in the code.
    Player *player = game_get_player(
        worker->game_copy, game_get_player_on_turn_index(worker->game_copy));
    Player *opponent =
        game_get_player(worker->game_copy,
                        1 - game_get_player_on_turn_index(worker->game_copy));

    int32_t spread = player_get_score(player) - player_get_score(opponent);
    // log_warn("returning final spread %d", spread);
    return spread;
  }

  PVLine child_pv;
  child_pv.game = worker->game_copy;

  int nplays;
  char *spaces = create_spaces(worker->solver->requested_plies - depth);
  bool arena_alloced = false;
  if (worker->current_iterative_deepening_depth != depth) {
    nplays = generate_stm_plays(worker);
    assign_estimates_and_sort(worker, depth, nplays);
    // log_warn("generated and allocated; nplays %d, cur_size %ld", nplays,
    //          worker->small_move_arena->size);
    arena_alloced = true;
  } else {
    // Use initial moves. They already have been sorted by estimated value.
    nplays = worker->solver->n_initial_moves;
  }
  // print_small_plays(worker, nplays, cur_move_loc);

  int32_t best_value = -LARGE_VALUE;
  int arena_offset =
      worker->small_move_arena->size - (sizeof(SmallMove) * nplays);

  SmallMove *small_moves =
      (SmallMove *)(worker->small_move_arena->memory + arena_offset);
  // log_warn("Iterating through %d plays", nplays);
  for (int idx = 0; idx < nplays; idx++) {
    SmallMove *small_move = &(small_moves[idx]);
    small_move_to_move(worker->move_list->spare_move, small_move,
                       game_get_board(worker->game_copy));

    // delete me
    StringBuilder *move_description = string_builder_create();
    string_builder_add_move_description(move_description,
                                        worker->move_list->spare_move,
                                        game_get_ld(worker->game_copy));
    log_warn("%sTrying moveidx %d, %s (tm:%x meta:%x)", spaces, idx,
             string_builder_peek(move_description), small_move->tiny_move,
             small_move->metadata);

    play_move_status_t play_status =
        play_move(worker->move_list->spare_move, worker->game_copy, NULL, NULL);
    assert(play_status == PLAY_MOVE_STATUS_SUCCESS);
    int32_t value = 0;
    if (idx == 0 || !worker->solver->negascout_optim) {
      value = negamax(worker, depth - 1, -beta, -alpha, &child_pv, pv_node);
    } else {
      value = negamax(worker, depth - 1, -alpha - 1, -alpha, &child_pv, false);
      if (alpha < -value && -value < beta) {
        // re-search with wider window
        value = negamax(worker, depth - 1, -beta, -alpha, &child_pv, pv_node);
      }
    }
    game_unplay_last_move(worker->game_copy);

    log_warn("%sNow unplayed %d, %s (tm:%x meta:%x)", spaces, idx,
             string_builder_peek(move_description), small_move->tiny_move,
             small_move->metadata);

    string_builder_destroy(move_description);

    if (-value > best_value) {
      best_value = -value;
      log_warn("%sUpdatePV, bestval %d", spaces, best_value);
      StringBuilder *child_pvsb =
          pvline_string(&child_pv, worker->game_copy, false);
      StringBuilder *old_pvsb = pvline_string(pv, worker->game_copy, false);
      pvline_update(pv, &child_pv, small_move,
                    best_value - worker->solver->initial_spread);

      StringBuilder *new_pvsb = pvline_string(pv, worker->game_copy, false);
      log_warn("%schild_pv: %s", spaces, string_builder_peek(child_pvsb));
      log_warn("%snew_pv: %s", spaces, string_builder_peek(new_pvsb));
      string_builder_destroy(child_pvsb);
      string_builder_destroy(old_pvsb);
      string_builder_destroy(new_pvsb);
    }
    if (worker->current_iterative_deepening_depth == depth) {
      // At the very top depth, set the estimated value of the small move,
      // for the next iterative deepening iteration.
      small_move_set_estimated_value(small_move, -value);
    }
    alpha = MAX(alpha, best_value);
    if (best_value >= beta) {
      // beta cut-off
      break;
    }
    // clear the child node's pv for the next child node
    pvline_clear(&child_pv);
  }
  if (arena_alloced) {
    arena_dealloc(worker->small_move_arena, nplays * sizeof(SmallMove));
    // log_warn("arena_dealloced; new size: %ld",
    // worker->small_move_arena->size); printf("%sReset back to %ld\n", spaces,
    // worker->small_move_arena->size);
  }
  free(spaces);
  return best_value;
}

void iterative_deepening(EndgameSolverWorker *worker, int plies) {

  int32_t alpha = -LARGE_VALUE;
  int32_t beta = LARGE_VALUE;

  if (worker->solver->first_win_optim) {
    // search a very small window centered around 0; we're just trying to find
    // something that surpasses it. This is useful to find "any win", and for
    // a pre-endgame solver.
    alpha = -1;
    beta = 1;
  }
  assert(worker->small_move_arena->size == 0); // make sure arena is empty.

  int initial_move_count = generate_stm_plays(worker);
  // Arena pointer better have started at 0, since it was empty.
  assign_estimates_and_sort(worker, 0, initial_move_count);
  worker->solver->initial_moves =
      (SmallMove *)(worker->small_move_arena->memory);
  worker->solver->n_initial_moves = initial_move_count;
  assert((size_t)worker->small_move_arena->size ==
         initial_move_count * sizeof(SmallMove));
  // log_warn("Generated %d initial moves; pointer is at %d",
  // initial_move_count,
  //          worker->small_move_arena->size);
  // SmallMove last_winner;

  worker->current_iterative_deepening_depth = 1;
  int start = 1;
  if (!worker->solver->iterative_deepening_optim) {
    start = plies;
  }

  for (int p = start; p <= plies; p++) {
    log_warn("Iterative deepening; ply %d", p);
    worker->current_iterative_deepening_depth = p;
    PVLine pv;
    pv.game = worker->game_copy;
    int32_t val = negamax(worker, p, alpha, beta, &pv, true);
    // sort moves by valuation for next time.
    qsort(worker->solver->initial_moves, initial_move_count, sizeof(SmallMove),
          compare_small_moves_by_estimated_value);

    // log_warn("val returned was %d, initial spread %d", val,
    //          worker->solver->initial_spread);
    worker->solver->best_pv_value = val - worker->solver->initial_spread;
    worker->solver->principal_variation = pv;
    log_warn("Best value so far: %d", worker->solver->best_pv_value);
  }
}

void *solver_worker(void *uncasted_solver_worker) {
  EndgameSolverWorker *solver_worker =
      (EndgameSolverWorker *)uncasted_solver_worker;
  EndgameSolver *solver = solver_worker->solver;
  // ThreadControl *thread_control = solver->thread_control;
  // later allow thread control to quit early.
  iterative_deepening(solver_worker, solver->requested_plies);
  log_trace("thread %d exiting", solver_worker->thread_index);
  return NULL;
}

PVLine endgame_solve(EndgameSolver *solver, int plies) {
  // bag must be empty. This should be validated by the caller.
  assert(!bag_get_tiles(game_get_bag(solver->game)));

  solver->requested_plies = plies;
  // kick-off iterative deepening thread.

  EndgameSolverWorker **solver_workers =
      malloc_or_die((sizeof(EndgameSolverWorker *)) * solver->threads);
  pthread_t *worker_ids =
      malloc_or_die((sizeof(pthread_t)) * (solver->threads));

  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    solver_workers[thread_index] =
        endgame_solver_create_worker(solver, thread_index);
    pthread_create(&worker_ids[thread_index], NULL, solver_worker,
                   solver_workers[thread_index]);
  }

  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    pthread_join(worker_ids[thread_index], NULL);
    solver_worker_destroy(solver_workers[thread_index]);
  }

  StringBuilder *pvsb =
      pvline_string(&solver->principal_variation, solver->game, true);
  log_warn("winner: %s", string_builder_peek(pvsb));
  string_builder_destroy(pvsb);

  free(solver_workers);
  free(worker_ids);

  return solver->principal_variation;
}