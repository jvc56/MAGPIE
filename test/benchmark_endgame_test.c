#include "benchmark_endgame_test.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/exec.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern bool move_gen_use_optimized_record_all_small;

static double get_time_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double run_endgames_baseline(Config *config, EndgameSolver *solver,
                                    int num_games, int ply, int base_seed) {
  move_gen_use_optimized_record_all_small = false;
  double total_time = 0;

  for (int i = 0; i < num_games; i++) {
    char autoplay_cmd[64];
    snprintf(autoplay_cmd, sizeof(autoplay_cmd), "autoplay games 1 -seed %d",
             base_seed + i);

    load_and_exec_config_or_die(config, "new");
    load_and_exec_config_or_die(config, autoplay_cmd);

    Game *game = config_get_game(config);
    while (bag_get_letters(game_get_bag(game)) > 0) {
      bag_draw_random_letter(game_get_bag(game), 0);
    }

    EndgameArgs args = {.game = game,
                        .thread_control = config_get_thread_control(config),
                        .plies = ply,
                        .tt_fraction_of_mem = 0.05,
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE};
    EndgameResults *results = config_get_endgame_results(config);
    ErrorStack *err = error_stack_create();

    double start = get_time_sec();
    endgame_solve(solver, &args, results, err);
    double end = get_time_sec();
    assert(error_stack_is_empty(err));
    total_time += (end - start);

    error_stack_destroy(err);

    if ((i + 1) % 50 == 0) {
      printf("  Baseline: %3d/%d (%.2fs)\n", i + 1, num_games, total_time);
    }
  }

  return total_time;
}

static double run_endgames_optimized(Config *config, EndgameSolver *solver,
                                     int num_games, int ply, int base_seed) {
  move_gen_use_optimized_record_all_small = true;
  double total_time = 0;

  for (int i = 0; i < num_games; i++) {
    char autoplay_cmd[64];
    snprintf(autoplay_cmd, sizeof(autoplay_cmd), "autoplay games 1 -seed %d",
             base_seed + i);

    load_and_exec_config_or_die(config, "new");
    load_and_exec_config_or_die(config, autoplay_cmd);

    Game *game = config_get_game(config);
    while (bag_get_letters(game_get_bag(game)) > 0) {
      bag_draw_random_letter(game_get_bag(game), 0);
    }

    EndgameArgs args = {.game = game,
                        .thread_control = config_get_thread_control(config),
                        .plies = ply,
                        .tt_fraction_of_mem = 0.05,
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE};
    EndgameResults *results = config_get_endgame_results(config);
    ErrorStack *err = error_stack_create();

    double start = get_time_sec();
    endgame_solve(solver, &args, results, err);
    double end = get_time_sec();
    assert(error_stack_is_empty(err));
    total_time += (end - start);

    error_stack_destroy(err);

    if ((i + 1) % 50 == 0) {
      printf("  Optimized: %3d/%d (%.2fs)\n", i + 1, num_games, total_time);
    }
  }

  return total_time;
}

void test_benchmark_endgame_multi_ply(void) {
  log_set_level(LOG_FATAL);

  const int num_games = 500;
  const int ply = 8;
  const int base_seed = 12345;

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  EndgameSolver *solver = endgame_solver_create();

  printf("\n");
  printf("==============================================\n");
  printf("  Endgame Benchmark: %d games, %d-ply\n", num_games, ply);
  printf("==============================================\n\n");

  // Run baseline
  printf("Running baseline (non-optimized)...\n");
  double base_time = run_endgames_baseline(config, solver, num_games, ply, base_seed);

  // Clear transposition table
  endgame_solver_clear_tt(solver);

  // Run optimized
  printf("\nRunning optimized...\n");
  double opt_time = run_endgames_optimized(config, solver, num_games, ply, base_seed);

  endgame_solver_destroy(solver);

  printf("\n");
  printf("==============================================\n");
  printf("  RESULTS\n");
  printf("==============================================\n");
  printf("  Games solved:      %d\n", num_games);
  printf("  Ply depth:         %d\n", ply);
  printf("  Baseline total:    %.4f s\n", base_time);
  printf("  Optimized total:   %.4f s\n", opt_time);
  printf("  Baseline avg:      %.4f ms/game\n", (base_time / num_games) * 1000);
  printf("  Optimized avg:     %.4f ms/game\n", (opt_time / num_games) * 1000);
  printf("  Speedup:           %.2f%%\n", (base_time / opt_time - 1.0) * 100.0);
  printf("==============================================\n\n");

  config_destroy(config);
}
