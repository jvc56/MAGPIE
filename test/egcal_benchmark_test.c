#include "../src/compat/ctime.h"
#include "../src/def/egcal_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/egcal_table.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>

enum {
  NUM_ENDGAMES = 10,
  SOLVE_PLIES = 6,
  NUM_MODES = 3,
};

typedef struct {
  int value;      // spread delta found
  double seconds; // solve time
} SolveResult;

static void solve_position(const Game *game, EndgameSolver *solver,
                            EndgameResults *results, ThreadControl *tc,
                            const EgcalTable *egcal_table,
                            int egcal_confidence_idx, SolveResult *out) {
  ErrorStack *error_stack = error_stack_create();
  EndgameArgs args = {0};
  args.game = game;
  args.plies = SOLVE_PLIES;
  args.num_threads = 1;
  args.tt_fraction_of_mem = 0.01;
  args.use_heuristics = true;
  args.thread_control = tc;
  args.num_top_moves = 1;
  args.egcal_table = egcal_table;
  args.egcal_confidence_idx = egcal_confidence_idx;

  Timer timer;
  ctimer_start(&timer);
  endgame_results_reset(results);
  endgame_solve(solver, &args, results, error_stack);
  ctimer_stop(&timer);

  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    out->value = 0;
    out->seconds = 0;
  } else {
    out->value =
        endgame_results_get_value(results, ENDGAME_RESULT_BEST);
    out->seconds = ctimer_elapsed_seconds(&timer);
  }
  error_stack_destroy(error_stack);
}

void test_egcal_benchmark(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 1");

  // Load egcal table
  ErrorStack *error_stack = error_stack_create();
  EgcalTable *egcal_table = egcal_table_load(
      "data/lexica/CSW21.egcal", error_stack);
  if (!egcal_table) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to load CSW21.egcal — run egcal first");
  }
  error_stack_destroy(error_stack);

  // Mode names and confidence indices (-1 = disabled)
  const char *mode_names[NUM_MODES] = {"baseline", "p90", "p99"};
  int confidence_idx[NUM_MODES] = {-1, 0, 2};

  // Create per-mode solvers
  EndgameSolver *solvers[NUM_MODES];
  EndgameResults *results[NUM_MODES];
  ThreadControl *tcs[NUM_MODES];
  for (int mode_idx = 0; mode_idx < NUM_MODES; mode_idx++) {
    solvers[mode_idx] = endgame_solver_create();
    results[mode_idx] = endgame_results_create();
    tcs[mode_idx] = thread_control_create();
  }

  // Accumulators
  double total_time[NUM_MODES] = {0};
  int total_value[NUM_MODES] = {0};
  int value_matches[NUM_MODES] = {0}; // matches baseline
  int games_solved = 0;

  // Generate and solve endgame positions
  Game *game = config_game_create(config);
  MoveList *move_list = move_list_create(500);
  XoshiroPRNG *prng = prng_create(12345);

  printf("\n%-4s | %-10s %6s | %-10s %6s | %-10s %6s\n", "game",
         mode_names[0], "time", mode_names[1], "time", mode_names[2], "time");
  printf("-----+-------------------+-------------------+-------------------\n");

  for (int game_idx = 0; game_idx < NUM_ENDGAMES; game_idx++) {
    // Play a random game to the endgame
    game_reset(game);
    game_seed(game, prng_next(prng));
    draw_starting_racks(game);

    while (!game_over(game) && !bag_is_empty(game_get_bag(game))) {
      const Move *move = get_top_equity_move(game, 0, move_list);
      play_move(move, game, NULL);
    }

    if (!bag_is_empty(game_get_bag(game)) || game_over(game)) {
      continue;
    }

    // Solve with each mode
    SolveResult solve_results[NUM_MODES];
    for (int mode_idx = 0; mode_idx < NUM_MODES; mode_idx++) {
      const EgcalTable *table =
          confidence_idx[mode_idx] >= 0 ? egcal_table : NULL;
      solve_position(game, solvers[mode_idx], results[mode_idx], tcs[mode_idx],
                     table, confidence_idx[mode_idx], &solve_results[mode_idx]);
      total_time[mode_idx] += solve_results[mode_idx].seconds;
      total_value[mode_idx] += solve_results[mode_idx].value;
    }

    // Check value matches vs baseline
    for (int mode_idx = 1; mode_idx < NUM_MODES; mode_idx++) {
      if (solve_results[mode_idx].value == solve_results[0].value) {
        value_matches[mode_idx]++;
      }
    }
    value_matches[0]++; // baseline always matches itself
    games_solved++;

    printf("%4d | %+5d %10.3fs | %+5d %10.3fs | %+5d %10.3fs\n",
           games_solved, solve_results[0].value, solve_results[0].seconds,
           solve_results[1].value, solve_results[1].seconds,
           solve_results[2].value, solve_results[2].seconds);
  }

  printf("\n=== Summary (%d endgames) ===\n\n", games_solved);
  printf("%-10s | total_time | avg_time  | match_rate | avg_value\n",
         "mode");
  printf("-----------+------------+-----------+------------+----------\n");
  for (int mode_idx = 0; mode_idx < NUM_MODES; mode_idx++) {
    printf("%-10s | %8.2fs  | %7.4fs  | %4d/%4d  | %+.1f\n",
           mode_names[mode_idx], total_time[mode_idx],
           total_time[mode_idx] / (double)games_solved,
           value_matches[mode_idx], games_solved,
           (double)total_value[mode_idx] / (double)games_solved);
  }
  printf("\nSpeedup p90 vs baseline: %.2fx\n",
         total_time[0] / total_time[1]);
  printf("Speedup p99 vs baseline: %.2fx\n",
         total_time[0] / total_time[2]);

  // Cleanup
  for (int mode_idx = 0; mode_idx < NUM_MODES; mode_idx++) {
    endgame_solver_destroy(solvers[mode_idx]);
    endgame_results_destroy(results[mode_idx]);
    thread_control_destroy(tcs[mode_idx]);
  }
  egcal_table_destroy(egcal_table);
  move_list_destroy(move_list);
  game_destroy(game);
  prng_destroy(prng);
  config_destroy(config);
}
