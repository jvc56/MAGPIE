#include "peg_fit.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../compat/ctime.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/bag.h"
#include "../ent/data_filepaths.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/peg_adjust.h"
#include "../ent/player.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "endgame.h"
#include "gameplay.h"
#include "move_gen.h"
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  PEG_FIT_INITIAL_MOVE_LIST_CAPACITY = 1024,
  PEG_FIT_INITIAL_POSITIONS_CAPACITY = 1024,
  PEG_FIT_MAX_SOLVE_ITERATIONS = 10000,
  // Progress is printed this many times over a run.
  PEG_FIT_PROGRESS_REPORTS = 20,
};

// The fit stops once no class term moves by more than this many points.
#define PEG_FIT_SOLVE_TOLERANCE 1e-9

typedef struct PegFitShared {
  const PegFitArgs *args;
  // The pre-endgame adjustments both players play with, or NULL for none.
  const PegAdjust *table;
  TranspositionTable *endgame_tt;
  atomic_int next_game;
  cpthread_mutex_t mutex;
  PegFitPosition *positions;
  int number_of_positions;
  int positions_capacity;
  int games_completed;
  int64_t rollouts_played;
  Timer timer;
} PegFitShared;

typedef struct PegFitWorker {
  PegFitShared *shared;
  Game *game;
  Game *rollout_game;
  MoveList *all_moves;
  MoveList *best_move;
  EndgameCtx *endgame_ctx;
  EndgameResults *endgame_results;
} PegFitWorker;

static uint64_t peg_fit_mix_seed(uint64_t seed, uint64_t first,
                                 uint64_t second) {
  uint64_t mixed =
      seed ^ (first * 0x9E3779B97F4A7C15ULL) ^ (second * 0xBF58476D1CE4E5B9ULL);
  mixed ^= mixed >> 31;
  mixed *= 0x94D049BB133111EBULL;
  mixed ^= mixed >> 29;
  return mixed;
}

static void peg_fit_generate_all_moves(const Game *game, MoveList **move_list) {
  while (true) {
    const MoveGenArgs args = {
        .game = game,
        .move_list = *move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
    };
    generate_moves(&args);
    const int capacity = move_list_get_capacity(*move_list);
    if (move_list_get_count(*move_list) < capacity) {
      return;
    }
    move_list_resize(*move_list, capacity * 2);
  }
}

// The move's class in a fit of number_of_values values: the table index a
// tile placement reaches, or the base class, number_of_values.
static int peg_fit_get_move_class(const Move *move, int number_of_tiles_in_bag,
                                  int number_of_values) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return number_of_values;
  }
  const int index =
      number_of_tiles_in_bag - move_get_tiles_played(move) + RACK_SIZE;
  return index < number_of_values ? index : number_of_values;
}

static Equity peg_fit_get_spread(const Game *game, int player_index) {
  return player_get_score(game_get_player(game, player_index)) -
         player_get_score(game_get_player(game, 1 - player_index));
}

// The final spread of player_index when the endgame in game is solved
// endgame_plies deep on the calling thread.
static Equity peg_fit_solve_endgame(PegFitWorker *worker, const Game *game,
                                    int player_index, uint64_t seed) {
  ThreadControl *thread_control = thread_control_create();
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  EndgameArgs endgame_args;
  endgame_args_fill(
      thread_control, game, /*tt_fraction_of_mem=*/0.0,
      worker->shared->args->endgame_plies,
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, /*num_threads=*/1,
      /*use_heuristics=*/true, /*num_top_moves=*/1,
      /*per_ply_callback=*/NULL, /*per_ply_callback_data=*/NULL,
      /*before_search_callback=*/NULL, /*before_search_callback_data=*/NULL,
      /*per_root_move_callback=*/NULL, /*per_root_move_callback_data=*/NULL,
      DUAL_LEXICON_MODE_IGNORANT, /*forced_pass_bypass=*/true,
      /*incremental_movegen=*/true, /*disable_best_reply_shortcuts=*/false,
      /*enable_pv_display=*/false, /*soft_time_limit=*/0.0,
      /*hard_time_limit=*/0.0, seed, /*skip_word_pruning=*/false,
      worker->shared->endgame_tt, /*max_workers=*/0, /*first_win=*/false,
      /*first_win_fallback_moves=*/0, /*use_initial_window=*/false,
      /*initial_alpha=*/0, /*initial_beta=*/0, /*external_deadline_ns=*/0,
      /*actual_move=*/NULL, &endgame_args);
  endgame_solve_inline(&worker->endgame_ctx, &endgame_args,
                       worker->endgame_results);
  thread_control_destroy(thread_control);
  const int solver_spread = endgame_results_get_spread(
      worker->endgame_results, ENDGAME_RESULT_BEST, game);
  return int_to_equity(game_get_player_on_turn_index(game) == player_index
                           ? solver_spread
                           : -solver_spread);
}

// The mover's spread change from playing move and then playing on to the end
// of the game, with the opponent's rack and the bag order drawn by seed.
static Equity peg_fit_rollout(PegFitWorker *worker, const Move *move,
                              uint64_t seed) {
  Game *game = worker->rollout_game;
  game_copy(game, worker->game);
  const int mover = game_get_player_on_turn_index(game);
  const Equity spread_before = peg_fit_get_spread(game, mover);
  return_rack_to_bag(game, 1 - mover);
  game_seed(game, seed);
  draw_to_full_rack(game, 1 - mover);
  play_move(move, game, NULL);
  while (!game_over(game)) {
    if (worker->shared->args->endgame_plies > 0 &&
        bag_get_letters(game_get_bag(game)) == 0) {
      return peg_fit_solve_endgame(worker, game, mover, seed) - spread_before;
    }
    play_move(get_top_equity_move(game, worker->best_move), game, NULL);
  }
  return peg_fit_get_spread(game, mover) - spread_before;
}

static void peg_fit_add_position(PegFitShared *shared,
                                 const PegFitPosition *position,
                                 int number_of_classes) {
  cpthread_mutex_lock(&shared->mutex);
  if (shared->number_of_positions == shared->positions_capacity) {
    shared->positions_capacity *= 2;
    shared->positions = realloc_or_die(
        shared->positions, sizeof(PegFitPosition) * shared->positions_capacity);
  }
  shared->positions[shared->number_of_positions++] = *position;
  shared->rollouts_played +=
      (int64_t)number_of_classes * shared->args->rollouts_per_class;
  cpthread_mutex_unlock(&shared->mutex);
}

static void peg_fit_analyze_position(PegFitWorker *worker, int game_idx,
                                     int turn) {
  const PegFitArgs *args = worker->shared->args;
  const Game *game = worker->game;
  const int number_of_tiles_in_bag = bag_get_letters(game_get_bag(game));
  peg_fit_generate_all_moves(game, &worker->all_moves);
  Move best_moves[PEG_FIT_MAX_NUMBER_OF_CLASSES];
  Equity best_static_equities[PEG_FIT_MAX_NUMBER_OF_CLASSES];
  PegFitPosition position = {.game_index = game_idx, .turn = turn};
  int number_of_classes = 0;
  const int number_of_moves = move_list_get_count(worker->all_moves);
  for (int move_idx = 0; move_idx < number_of_moves; move_idx++) {
    const Move *move = move_list_get_move(worker->all_moves, move_idx);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    const int move_class = peg_fit_get_move_class(move, number_of_tiles_in_bag,
                                                  args->number_of_values);
    // Without the playing table, which may reach plays the fit leaves in the
    // base class.
    Equity static_equity = move_get_equity(move);
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      static_equity -= peg_adjust_get_placement_value(
          worker->shared->table, number_of_tiles_in_bag,
          move_get_tiles_played(move));
    }
    if (!position.has_class[move_class]) {
      number_of_classes++;
    } else if (static_equity <= best_static_equities[move_class]) {
      continue;
    }
    position.has_class[move_class] = true;
    best_static_equities[move_class] = static_equity;
    move_copy(&best_moves[move_class], move);
  }
  if (number_of_classes < 2) {
    return;
  }
  for (int class_idx = 0; class_idx <= args->number_of_values; class_idx++) {
    if (!position.has_class[class_idx]) {
      continue;
    }
    double total = 0;
    for (int rollout_idx = 0; rollout_idx < args->rollouts_per_class;
         rollout_idx++) {
      // The same draws for every class of the position.
      const uint64_t seed = peg_fit_mix_seed(
          args->seed, ((uint64_t)game_idx << 8) | (uint64_t)turn,
          (uint64_t)rollout_idx);
      total += equity_to_double(
          peg_fit_rollout(worker, &best_moves[class_idx], seed));
    }
    position.value[class_idx] =
        total / args->rollouts_per_class -
        equity_to_double(best_static_equities[class_idx]);
  }
  peg_fit_add_position(worker->shared, &position, number_of_classes);
}

static bool peg_fit_interrupted(const PegFitShared *shared) {
  return thread_control_get_status(shared->args->thread_control) ==
         THREAD_CONTROL_STATUS_USER_INTERRUPT;
}

static void peg_fit_complete_game(PegFitShared *shared) {
  const PegFitArgs *args = shared->args;
  cpthread_mutex_lock(&shared->mutex);
  shared->games_completed++;
  int report_interval = args->number_of_games / PEG_FIT_PROGRESS_REPORTS;
  if (report_interval < 1) {
    report_interval = 1;
  }
  if (shared->games_completed % report_interval == 0) {
    thread_control_print_formatted(
        args->thread_control,
        "pegfit: %d of %d games, %d positions, %lld rollouts (%.1fs)\n",
        shared->games_completed, args->number_of_games,
        shared->number_of_positions, (long long)shared->rollouts_played,
        ctimer_elapsed_seconds(&shared->timer));
  }
  cpthread_mutex_unlock(&shared->mutex);
}

static void *peg_fit_worker_run(void *arg) {
  PegFitWorker *worker = (PegFitWorker *)arg;
  PegFitShared *shared = worker->shared;
  const PegFitArgs *args = shared->args;
  while (!peg_fit_interrupted(shared)) {
    const int game_idx = atomic_fetch_add(&shared->next_game, 1);
    if (game_idx >= args->number_of_games) {
      break;
    }
    Game *game = worker->game;
    game_reset(game);
    game_seed(game, peg_fit_mix_seed(args->seed, (uint64_t)game_idx, 0));
    draw_starting_racks(game);
    int turn = 0;
    while (!game_over(game) && !peg_fit_interrupted(shared)) {
      const int number_of_tiles_in_bag = bag_get_letters(game_get_bag(game));
      if (number_of_tiles_in_bag > 0 &&
          number_of_tiles_in_bag < args->number_of_values) {
        peg_fit_analyze_position(worker, game_idx, turn);
      }
      play_move(get_top_equity_move(game, worker->best_move), game, NULL);
      turn++;
    }
    peg_fit_complete_game(shared);
  }
  return NULL;
}

void peg_fit_solve(const PegFitPosition *positions, int number_of_positions,
                   int number_of_values, PegFitResult *result) {
  const int number_of_classes = number_of_values + 1;
  double *position_terms =
      calloc_or_die((size_t)number_of_positions + 1, sizeof(double));
  for (int class_idx = 0; class_idx < PEG_FIT_MAX_NUMBER_OF_CLASSES;
       class_idx++) {
    result->terms[class_idx] = 0;
    result->standard_errors[class_idx] = 0;
    result->observations[class_idx] = 0;
  }
  for (int class_idx = 0; class_idx < number_of_classes; class_idx++) {
    for (int pos_idx = 0; pos_idx < number_of_positions; pos_idx++) {
      if (positions[pos_idx].has_class[class_idx]) {
        result->observations[class_idx]++;
      }
    }
  }
  // Alternating exact minimizations over the position terms and the class
  // terms, until the class terms settle.
  for (int iteration = 0; iteration < PEG_FIT_MAX_SOLVE_ITERATIONS;
       iteration++) {
    for (int pos_idx = 0; pos_idx < number_of_positions; pos_idx++) {
      const PegFitPosition *position = &positions[pos_idx];
      double total = 0;
      int count = 0;
      for (int class_idx = 0; class_idx < number_of_classes; class_idx++) {
        if (position->has_class[class_idx]) {
          total += position->value[class_idx] - result->terms[class_idx];
          count++;
        }
      }
      position_terms[pos_idx] = count > 0 ? total / count : 0;
    }
    double largest_change = 0;
    for (int class_idx = 0; class_idx < number_of_values; class_idx++) {
      if (result->observations[class_idx] == 0) {
        continue;
      }
      double total = 0;
      for (int pos_idx = 0; pos_idx < number_of_positions; pos_idx++) {
        const PegFitPosition *position = &positions[pos_idx];
        if (position->has_class[class_idx]) {
          total += position->value[class_idx] - position_terms[pos_idx];
        }
      }
      const double term = total / result->observations[class_idx];
      const double change = fabs(term - result->terms[class_idx]);
      if (change > largest_change) {
        largest_change = change;
      }
      result->terms[class_idx] = term;
    }
    if (largest_change < PEG_FIT_SOLVE_TOLERANCE) {
      break;
    }
  }
  // Each class term's standard error, from its residuals.
  for (int class_idx = 0; class_idx < number_of_classes; class_idx++) {
    const int count = result->observations[class_idx];
    if (count < 2) {
      continue;
    }
    double sum_of_squares = 0;
    for (int pos_idx = 0; pos_idx < number_of_positions; pos_idx++) {
      const PegFitPosition *position = &positions[pos_idx];
      if (position->has_class[class_idx]) {
        const double residual = position->value[class_idx] -
                                position_terms[pos_idx] -
                                result->terms[class_idx];
        sum_of_squares += residual * residual;
      }
    }
    result->standard_errors[class_idx] =
        sqrt(sum_of_squares / (count - 1) / count);
  }
  free(position_terms);
}

static int peg_fit_compare_positions(const void *first, const void *second) {
  const PegFitPosition *first_position = (const PegFitPosition *)first;
  const PegFitPosition *second_position = (const PegFitPosition *)second;
  if (first_position->game_index != second_position->game_index) {
    return first_position->game_index < second_position->game_index ? -1 : 1;
  }
  if (first_position->turn != second_position->turn) {
    return first_position->turn < second_position->turn ? -1 : 1;
  }
  return 0;
}

static void peg_fit_write_table(const PegFitArgs *args,
                                const PegFitShared *shared,
                                const PegFitResult *result,
                                ErrorStack *error_stack) {
  char *filename = data_filepaths_get_writable_filename(
      args->data_paths, args->output_name, DATA_FILEPATH_TYPE_PEG_ADJUST,
      error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb,
      "# Pre-endgame adjustments fitted by pegfit: playing table %s, %d "
      "games, %d positions, %d rollouts per class, endgame plies %d, seed "
      "%llu.\n"
      "# index,value (points)\n",
      shared->table != NULL ? peg_adjust_get_name(shared->table) : "none",
      args->number_of_games, shared->number_of_positions,
      args->rollouts_per_class, args->endgame_plies,
      (unsigned long long)args->seed);
  for (int class_idx = 1; class_idx < args->number_of_values; class_idx++) {
    string_builder_add_formatted_string(sb, "%d,%.2f\n", class_idx,
                                        result->terms[class_idx]);
  }
  write_string_to_file(filename, "w", string_builder_peek(sb), error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print_formatted(args->thread_control, "pegfit: wrote %s\n",
                                   filename);
  }
  string_builder_destroy(sb);
  free(filename);
}

static void peg_fit_print_result(const PegFitArgs *args,
                                 const PegFitShared *shared,
                                 const PegFitResult *result) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb,
      "pegfit: %d games, %d positions, %lld rollouts (%.1fs)\n"
      "index    term     SE  positions  playing\n",
      shared->games_completed, shared->number_of_positions,
      (long long)shared->rollouts_played,
      ctimer_elapsed_seconds(&shared->timer));
  const int base_class = args->number_of_values;
  for (int class_idx = 1; class_idx <= base_class; class_idx++) {
    if (class_idx == base_class) {
      string_builder_add_string(sb, " base");
    } else {
      string_builder_add_formatted_string(sb, "%5d", class_idx);
    }
    const double playing_value =
        class_idx == base_class || shared->table == NULL
            ? 0
            : equity_to_double(peg_adjust_get_value(shared->table, class_idx));
    string_builder_add_formatted_string(
        sb, "  %6.2f  %5.2f  %9d  %7.2f\n", result->terms[class_idx],
        result->standard_errors[class_idx], result->observations[class_idx],
        playing_value);
  }
  thread_control_print_formatted(args->thread_control, "%s",
                                 string_builder_peek(sb));
  string_builder_destroy(sb);
}

void peg_fit(const PegFitArgs *args, ErrorStack *error_stack) {
  PegFitShared shared = {
      .args = args,
      .table = player_get_peg_adjust(game_get_player(args->game, 0)),
      .endgame_tt = NULL,
      .positions = malloc_or_die(sizeof(PegFitPosition) *
                                 PEG_FIT_INITIAL_POSITIONS_CAPACITY),
      .number_of_positions = 0,
      .positions_capacity = PEG_FIT_INITIAL_POSITIONS_CAPACITY,
      .games_completed = 0,
      .rollouts_played = 0,
  };
  if (args->endgame_plies > 0) {
    shared.endgame_tt = transposition_table_create(args->tt_fraction_of_mem);
  }
  atomic_init(&shared.next_game, 0);
  cpthread_mutex_init(&shared.mutex);
  ctimer_start(&shared.timer);

  const int number_of_threads = args->number_of_threads;
  PegFitWorker *workers =
      malloc_or_die(sizeof(PegFitWorker) * (size_t)number_of_threads);
  cpthread_t *threads =
      malloc_or_die(sizeof(cpthread_t) * (size_t)number_of_threads);
  for (int thread_idx = 0; thread_idx < number_of_threads; thread_idx++) {
    Game *game = game_duplicate(args->game);
    workers[thread_idx] = (PegFitWorker){
        .shared = &shared,
        .game = game,
        .rollout_game = game_duplicate(game),
        .all_moves = move_list_create(PEG_FIT_INITIAL_MOVE_LIST_CAPACITY),
        .best_move = move_list_create(1),
        .endgame_ctx = NULL,
        .endgame_results = endgame_results_create(),
    };
    cpthread_create(&threads[thread_idx], peg_fit_worker_run,
                    &workers[thread_idx]);
  }
  for (int thread_idx = 0; thread_idx < number_of_threads; thread_idx++) {
    cpthread_join(threads[thread_idx]);
    PegFitWorker *worker = &workers[thread_idx];
    game_destroy(worker->game);
    game_destroy(worker->rollout_game);
    move_list_destroy(worker->all_moves);
    move_list_destroy(worker->best_move);
    endgame_ctx_destroy(worker->endgame_ctx);
    endgame_results_destroy(worker->endgame_results);
  }
  free(threads);
  free(workers);
  transposition_table_destroy(shared.endgame_tt);

  if (!peg_fit_interrupted(&shared)) {
    qsort(shared.positions, (size_t)shared.number_of_positions,
          sizeof(PegFitPosition), peg_fit_compare_positions);
    PegFitResult result;
    peg_fit_solve(shared.positions, shared.number_of_positions,
                  args->number_of_values, &result);
    peg_fit_print_result(args, &shared, &result);
    peg_fit_write_table(args, &shared, &result, error_stack);
  }
  free(shared.positions);
}
