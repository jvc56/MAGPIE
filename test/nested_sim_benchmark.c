// Benchmark comparing static eval vs nested sim ply strategy.
// Usage: ./bin/magpie_test nestsimbench
//
// Runs simulation on a mid-game position with three configurations:
//   1. Static eval (existing behavior): 15s time limit
//   2. Nested sim (K=5, N=8, plies=4): 15s time limit
//   3. Multi-fidelity (5s static screening + 10s nested refinement)
// Reports: best play, win%, iterations, node count, wall time.

#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/sim_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>

// Mid-game position with interesting choices
#define BENCHMARK_CGP DELDAR_VS_HARSHAN_CGP

static void print_sim_results(const char *label, SimResults *sim_results,
                              double elapsed_s, const Config *config) {
  const int num_plays = sim_results_get_number_of_plays(sim_results);
  printf("\n=== %s ===\n", label);
  printf("  Elapsed:    %.2f s\n", elapsed_s);
  printf("  Iterations: %lu\n",
         (unsigned long)sim_results_get_iteration_count(sim_results));
  printf("  Nodes:      %lu\n",
         (unsigned long)sim_results_get_node_count(sim_results));
  printf("  Plays:      %d\n", num_plays);
  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  printf("  BAI status: %d\n", bai_result_get_status(bai_result));
  printf("  Best arm:   %d\n", bai_result_get_best_arm(bai_result));

  StringBuilder *sb = string_builder_create();
  const Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  // Print top plays sorted by win%
  printf("  %-4s %-20s %8s %8s %8s\n", "Rank", "Move", "Win%", "Eq",
         "Iters");
  for (int i = 0; i < num_plays && i < 10; i++) {
    const SimmedPlay *sp = sim_results_get_simmed_play(sim_results, i);
    const Move *move = simmed_play_get_move(sp);
    string_builder_clear(sb);
    string_builder_add_move(sb, board, move, ld, false);
    const double win_pct =
        stat_get_mean(simmed_play_get_win_pct_stat(sp)) * 100.0;
    const double eq = stat_get_mean(simmed_play_get_equity_stat(sp));
    const uint64_t iters =
        stat_get_num_samples(simmed_play_get_win_pct_stat(sp));
    printf("  %-4d %-20s %7.2f%% %8.2f %8lu\n", i + 1,
           string_builder_peek(sb), win_pct, eq, (unsigned long)iters);
  }
  string_builder_destroy(sb);
}

static void run_benchmark(Config *config, const char *label,
                          int num_fidelity_levels,
                          const FidelityLevel *levels, int num_threads) {
  SimResults *sim_results = config_get_sim_results(config);
  ThreadControl *tc = config_get_thread_control(config);

  // Use config_simulate for a 1-iteration throwaway to ensure win_pcts are
  // loaded, then we use config_get_win_pcts for the custom SimArgs.
  // This is a no-op if win_pcts are already loaded.
  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
  ErrorStack *error_stack = error_stack_create();
  config_simulate(config, NULL, NULL, sim_results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    error_stack_destroy(error_stack);
    return;
  }

  // Build custom SimArgs with our fidelity levels
  SimArgs args;
  sim_args_fill(
      /* num_plies */ 6, config_get_move_list(config),
      /* known_opp_rack */ NULL, config_get_win_pcts(config),
      /* inference_results */ NULL, tc, config_get_game(config),
      /* sim_with_inference */ false,
      /* use_heat_map */ false, num_threads,
      /* print_interval */ 0,
      /* max_num_display_plays */ 15,
      /* max_num_display_plies */ 6,
      /* seed */ 42,
      /* max_iterations */ UINT64_MAX,
      /* min_play_iterations */ 100,
      /* scond */ 99.5, BAI_THRESHOLD_GK16,
      /* time_limit_seconds */ levels[0].time_limit_seconds,
      BAI_SAMPLING_RULE_TOP_TWO_IDS,
      /* cutoff */ 0.01,
      /* inference_args */ NULL, &args);

  // Override fidelity levels
  args.num_fidelity_levels = num_fidelity_levels;
  for (int i = 0; i < num_fidelity_levels; i++) {
    args.fidelity_levels[i] = levels[i];
  }

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  simulate_without_ctx(&args, sim_results, error_stack);

  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed_s =
      (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
  }
  error_stack_destroy(error_stack);

  print_sim_results(label, sim_results, elapsed_s, config);
}

void test_nested_sim_benchmark(void) {
  const int num_threads = 16;
  const int time_limit_s = 15;

  printf("\n");
  printf("========================================\n");
  printf("  Nested Sim Benchmark\n");
  printf("  Threads: %d, Time limit: %ds\n", num_threads, time_limit_s);
  printf("  Position: DELDAR_VS_HARSHAN_CGP\n");
  printf("========================================\n");

  // Set up game position with lots of candidate moves
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 6 -threads 16 -it 1 -minp 1 -scond none");
  load_and_exec_config_or_die(config, "cgp " BENCHMARK_CGP);
  load_and_exec_config_or_die(config, "gen");

  const MoveList *ml = config_get_move_list(config);
  printf("  Generated %d candidate moves\n", move_list_get_count(ml));

  // --- Benchmark 1: Static eval only ---
  FidelityLevel static_level = {
      .sample_limit = UINT64_MAX,
      .sample_minimum = 100,
      .time_limit_seconds = time_limit_s,
      .ply_strategy = PLY_STRATEGY_STATIC,
  };
  run_benchmark(config, "STATIC EVAL (baseline)", 1, &static_level,
                num_threads);

  // --- Benchmark 2: Nested sim (K=5, N=8, plies=4) ---
  FidelityLevel nested_level = {
      .sample_limit = UINT64_MAX,
      .sample_minimum = 50,
      .time_limit_seconds = time_limit_s,
      .ply_strategy = PLY_STRATEGY_NESTED_SIM,
      .nested_candidates = 5,
      .nested_rollouts = 8,
      .nested_plies = 4,
  };
  run_benchmark(config, "NESTED SIM (K=5 N=8 plies=4)", 1, &nested_level,
                num_threads);

  // --- Benchmark 3: Multi-fidelity (static screening + nested refinement) ---
  FidelityLevel mf_levels[2] = {
      {
          .sample_limit = UINT64_MAX,
          .sample_minimum = 100,
          .time_limit_seconds = 5,
          .ply_strategy = PLY_STRATEGY_STATIC,
      },
      {
          .sample_limit = UINT64_MAX,
          .sample_minimum = 50,
          .time_limit_seconds = 10,
          .ply_strategy = PLY_STRATEGY_NESTED_SIM,
          .nested_candidates = 5,
          .nested_rollouts = 8,
          .nested_plies = 4,
      },
  };
  run_benchmark(config, "MULTI-FIDELITY (5s static + 10s nested)", 2,
                mf_levels, num_threads);

  printf("\n========================================\n");
  printf("  Benchmark complete\n");
  printf("========================================\n");

  config_destroy(config);
}
