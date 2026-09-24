#include "peg_speed_bench_test.h"

#include "../src/compat/ctime.h"
#include "../src/ent/game.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>

// Fixed-work PEG timing benchmark. Each entry point solves the positions of a
// committed notes/peg_positions fixture with the peg command's default
// arguments (config_fill_peg_args) and no time budget, so two builds do
// identical work and their wall times compare directly. Prints one row per
// position (seconds, best move, win, spread, last completed stage) and a total
// line; the PEG result on each row should be identical between the two builds.
//
// As in benchmark_peg_test.c there are deliberately no environment-variable
// knobs: the fixture, position count, thread count and stage schedule are
// hardcoded in the entry points below — edit them in source to sweep
// different values.

typedef struct PegSpeedBenchConfig {
  const char *cgp_file;
  int max_positions;
  int num_threads;
  const int *stage_top_k; // NULL = the peg command's default cascade
  int num_stages;
} PegSpeedBenchConfig;

static void run_peg_speed_bench(const PegSpeedBenchConfig *cfg) {
  log_set_level(LOG_FATAL);
  char settings[128];
  (void)snprintf(settings, sizeof(settings),
                 "set -lex CSW24 -threads %d -pegoutcomes false",
                 cfg->num_threads);
  Config *config = config_create_or_die(settings);
  load_and_exec_config_or_die(config, "new");
  FILE *file = fopen_or_die(cfg->cgp_file, "re");
  printf("  PEG fixed-work timing [%s]: up to %d positions, %d threads, %d "
         "stages\n",
         cfg->cgp_file, cfg->max_positions, cfg->num_threads, cfg->num_stages);
  printf("  %4s  %10s  %-16s  %7s  %8s  %5s\n", "Pos", "seconds", "move", "win",
         "spread", "stage");
  char line[4096];
  int index = 0;
  double total_seconds = 0;
  while (index < cfg->max_positions && fgets(line, sizeof(line), file)) {
    if (line[0] == '\n' || line[0] == '\0') {
      continue;
    }
    ErrorStack *error_stack = error_stack_create();
    game_load_cgp(config_get_game(config), line, error_stack);
    assert(error_stack_is_empty(error_stack));
    PegArgs args;
    config_fill_peg_args(config, &args);
    args.time_budget_seconds = 0; // unlimited: fixed work
    if (cfg->num_stages > 0) {
      args.stage_top_k = cfg->stage_top_k;
      args.num_stages = cfg->num_stages;
    }
    args.include_per_scenario = false;
    PegResult result;
    Timer timer;
    ctimer_start(&timer);
    peg_solve(&args, &result, error_stack);
    const double elapsed = ctimer_elapsed_seconds(&timer);
    assert(error_stack_is_empty(error_stack));
    StringBuilder *move_sb = string_builder_create();
    string_builder_add_move(move_sb, game_get_board(config_get_game(config)),
                            &result.best_move,
                            game_get_ld(config_get_game(config)), false);
    printf("  %4d  %10.3f  %-16s  %7.4f  %+8.3f  %5d\n", index, elapsed,
           string_builder_peek(move_sb), result.best_win, result.best_spread,
           result.last_completed_stage);
    (void)fflush(stdout);
    string_builder_destroy(move_sb);
    peg_result_destroy(&result);
    error_stack_destroy(error_stack);
    total_seconds += elapsed;
    index++;
  }
  fclose_or_die(file);
  printf("  total: positions=%d threads=%d stages=%d seconds=%.3f\n", index,
         cfg->num_threads, cfg->num_stages, total_seconds);
  config_destroy(config);
}

// Stage schedule 4,2 keeps a position to seconds (1 in bag) or about a minute
// (2 in bag) rather than the default cascade's minutes.
void test_peg_speed_bench_1(void) {
  static const int stage_top_k[] = {4, 2};
  const PegSpeedBenchConfig cfg = {
      .cgp_file = "notes/peg_positions/random_1peg.txt",
      .max_positions = 24,
      .num_threads = 9,
      .stage_top_k = stage_top_k,
      .num_stages = 2,
  };
  run_peg_speed_bench(&cfg);
}

void test_peg_speed_bench_2(void) {
  static const int stage_top_k[] = {4, 2};
  const PegSpeedBenchConfig cfg = {
      .cgp_file = "notes/peg_positions/random_2peg.txt",
      .max_positions = 12,
      .num_threads = 9,
      .stage_top_k = stage_top_k,
      .num_stages = 2,
  };
  run_peg_speed_bench(&cfg);
}
