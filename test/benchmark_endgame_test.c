#include "benchmark_endgame_test.h"

#include "../src/def/game_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/str/game_string.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// Execute config command quietly (suppress stdout during execution)
static void exec_config_quiet(Config *config, const char *cmd) {
  // Suppress stdout
  fflush(stdout);
  int saved_stdout = dup(STDOUT_FILENO);
  int devnull = open("/dev/null", O_WRONLY);
  dup2(devnull, STDOUT_FILENO);
  close(devnull);

  ErrorStack *error_stack = error_stack_create();
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  config_load_command(config, cmd, error_stack);
  assert(error_stack_is_empty(error_stack));
  config_execute_command(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_FINISHED);

  // Restore stdout
  fflush(stdout);
  dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
}

static double get_time_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Play moves until the bag is empty, returning true if we get a valid endgame
// position (bag empty, both players have tiles)
static bool play_until_bag_empty(Game *game, MoveList *move_list) {
  while (bag_get_letters(game_get_bag(game)) > 0) {
    Move *move = get_top_equity_move(game, 0, move_list);
    play_move(move, game, NULL);

    // Check if game ended before bag emptied
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      return false;
    }
  }

  // Verify valid endgame: bag empty, both players have tiles
  const Rack *rack0 = player_get_rack(game_get_player(game, 0));
  const Rack *rack1 = player_get_rack(game_get_player(game, 1));
  return !rack_is_empty(rack0) && !rack_is_empty(rack1);
}

static double run_endgames(Config *config, EndgameSolver *solver, int num_games,
                           int ply, int num_threads, uint64_t base_seed,
                           bool verbose) {
  double total_time = 0;
  int valid_endgames = 0;

  MoveList *move_list = move_list_create(1);

  // Create the initial game (required before game_reset works)
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  for (int i = 0; i < num_games; i++) {
    // Reset game directly (avoids spurious output from "new" command)
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    if (verbose) {
      // Print the endgame position
      printf("\n--- Game %d (seed %llu) ---\n", valid_endgames + 1,
             (unsigned long long)(base_seed + (uint64_t)i));
      StringBuilder *game_sb = string_builder_create();
      GameStringOptions *gso = game_string_options_create_default();
      string_builder_add_game(game, NULL, gso, NULL, game_sb);
      printf("%s", string_builder_peek(game_sb));
      string_builder_destroy(game_sb);
      game_string_options_destroy(gso);
    }

    EndgameArgs args = {.game = game,
                        .thread_control = config_get_thread_control(config),
                        .plies = ply,
                        .tt_fraction_of_mem = 0.05,
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        .num_threads = num_threads};
    EndgameResults *results = config_get_endgame_results(config);
    ErrorStack *err = error_stack_create();

    if (verbose) {
      printf("Solving %d-ply endgame with %d thread(s)...\n", ply, num_threads);
    }
    double start = get_time_sec();
    endgame_solve(solver, &args, results, err);
    double end = get_time_sec();
    assert(error_stack_is_empty(err));
    double elapsed = end - start;
    total_time += elapsed;
    valid_endgames++;
    if (verbose) {
      printf("  Solved in %.3f ms\n", elapsed * 1000);
      StringBuilder *pv_sb = string_builder_create();
      string_builder_endgame_results(pv_sb, results, game, NULL, false);
      printf("%s\n", string_builder_peek(pv_sb));
      string_builder_destroy(pv_sb);
    }

    error_stack_destroy(err);
  }

  move_list_destroy(move_list);

  if (verbose) {
    printf("  Valid endgames: %d/%d\n", valid_endgames, num_games);
  }
  return total_time;
}

void test_benchmark_endgame(void) {
  log_set_level(LOG_FATAL);

  const int num_games = 25;
  const int ply = 12;
  const uint64_t base_seed = 0;
  const int thread_counts[] = {10};
  const int num_thread_counts = sizeof(thread_counts) / sizeof(thread_counts[0]);

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  printf("\n");
  printf("==============================================\n");
  printf("  Endgame Benchmark: %d games, %d-ply\n", num_games, ply);
  printf("  Testing thread counts: 10 (YBWC)\n");
  printf("==============================================\n\n");

  double times[num_thread_counts];

  // Run benchmarks for each thread count (fresh solver each time for fair comparison)
  for (int i = 0; i < num_thread_counts; i++) {
    int threads = thread_counts[i];
    EndgameSolver *solver = endgame_solver_create();
    printf("--- %d threads (YBWC) ---\n", threads);
    times[i] = run_endgames(config, solver, num_games, ply, threads,
                            base_seed, true);
    printf("  Total time:    %.4f s\n", times[i]);
    printf("  Average:       %.4f ms/game\n", (times[i] / num_games) * 1000);
    printf("\n");
    endgame_solver_destroy(solver);
  }

  printf("==============================================\n");
  printf("  RESULTS SUMMARY\n");
  printf("==============================================\n");
  printf("  Games:         %d\n", num_games);
  printf("  Ply depth:     %d\n", ply);
  printf("\n");
  printf("  Threads   Time (s)   ms/game\n");
  printf("  -------   --------   -------\n");
  for (int i = 0; i < num_thread_counts; i++) {
    int threads = thread_counts[i];
    printf("  %7d   %8.2f   %7.2f\n",
           threads, times[i], (times[i] / num_games) * 1000);
  }
  printf("==============================================\n\n");

  config_destroy(config);
}
