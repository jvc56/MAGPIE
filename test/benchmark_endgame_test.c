#include "benchmark_endgame_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/game_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
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
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Execute config command quietly (suppress stdout during execution)
static void exec_config_quiet(Config *config, const char *cmd) {
  // Suppress stdout
  (void)fflush(stdout);
  int saved_stdout = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
  int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
  (void)dup2(devnull, STDOUT_FILENO);
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
  (void)fflush(stdout);
  (void)dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
}

// Play moves until the bag is empty, returning true if we get a valid endgame
// position (bag empty, both players have tiles)
static bool play_until_bag_empty(Game *game, MoveList *move_list) {
  while (bag_get_letters(game_get_bag(game)) > 0) {
    const Move *move = get_top_equity_move(game, 0, move_list);
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

static void run_endgames_with_timing(Config *config, EndgameSolver *solver,
                                     int num_games, int ply,
                                     uint64_t base_seed) {
  MoveList *move_list = move_list_create(1);

  // Create the initial game (required before game_reset works)
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  int valid_endgames = 0;
  double total_time = 0;
  const int max_attempts =
      num_games * 10; // Safety limit to prevent infinite loop

  for (int i = 0; valid_endgames < num_games && i < max_attempts; i++) {
    // Reset game directly (avoids spurious output from "new" command)
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    // Print the endgame position
    printf("\n--- Game %d (seed %" PRIu64 ") ---\n", valid_endgames + 1,
           base_seed + (uint64_t)i);
    StringBuilder *game_sb = string_builder_create();
    GameStringOptions *gso = game_string_options_create_default();
    string_builder_add_game(game, NULL, gso, NULL, game_sb);
    printf("%s", string_builder_peek(game_sb));
    string_builder_destroy(game_sb);
    game_string_options_destroy(gso);

    Timer timer;
    ctimer_start(&timer);
    EndgameArgs args = {.game = game,
                        .thread_control = config_get_thread_control(config),
                        .plies = ply,
                        .tt_fraction_of_mem = 0.05,
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        .num_threads = 6,
                        .per_ply_callback = NULL,
                        .per_ply_callback_data = NULL};
    EndgameResults *results = config_get_endgame_results(config);
    ErrorStack *err = error_stack_create();

    printf("Solving %d-ply endgame...\n", ply);
    endgame_solve(solver, &args, results, err);
    double elapsed = ctimer_elapsed_seconds(&timer);
    total_time += elapsed;
    assert(error_stack_is_empty(err));

    // Print the result
    char *result_str = endgame_results_get_string(results, game, NULL, true);
    printf("%s", result_str);
    free(result_str);
    printf("Time: %.3fs\n", elapsed);

    valid_endgames++;
    error_stack_destroy(err);
  }

  // Ensure we found enough valid endgames
  assert(valid_endgames == num_games);

  printf("\n==============================================\n");
  printf("  TOTAL TIME: %.3fs for %d games\n", total_time, num_games);
  printf("  AVERAGE:    %.3fs per game\n", total_time / num_games);
  printf("==============================================\n");

  move_list_destroy(move_list);
}

void test_benchmark_endgame(void) {
  log_set_level(LOG_WARN); // Allow warnings to show diagnostics

  const int num_games = 100;
  const int ply = 3;
  const uint64_t base_seed = 0;

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");

  EndgameSolver *solver = endgame_solver_create();

  printf("\n");
  printf("==============================================\n");
  printf("  Endgame Benchmark: %d games, %d-ply\n", num_games, ply);
  printf("==============================================\n");

  run_endgames_with_timing(config, solver, num_games, ply, base_seed);

  endgame_solver_destroy(solver);
  config_destroy(config);
}

// Benchmark the 14domino endgame at 7 plies with different thread counts.
// Uses TT fraction 0.20 (~4GB on a 22GB system, capped at 2^28 entries).
void test_benchmark_threads(void) {
  log_set_level(LOG_WARN);

  // 14domino endgame position: ?AEEKSU vs BEIQUVW (14 tiles total)
  const char *cgp =
      "cgp "
      "6MOO1VIRLS/1EJECTA6A1/2I2AEON4R1/2BAH6X1N1/2SLID4GIFTS/"
      "4DONG1OR1R1i/7HOURLY1Z/FE4DINT1A2Y/RECLINE2I1N3/"
      "EW1ATAP2E1G3/M10U3/D3PATOOTIE3/15/15/15 "
      "?AEEKSU/BEIQUVW 276/321 0 -lex NWL23;";

  const int ply = 7;
  const double tt_fraction = 0.20;
  const int thread_counts[] = {4, 6, 8, 10, 12, 16};
  const int num_configs = 6;

  printf("\n");
  printf("==============================================================\n");
  printf("  Thread Benchmark: 14domino endgame at %d plies\n", ply);
  printf("  TT fraction: %.2f (targeting <=4GB)\n", tt_fraction);
  printf("==============================================================\n");

  for (int c = 0; c < num_configs; c++) {
    int num_threads = thread_counts[c];

    char config_str[256];
    snprintf(config_str, sizeof(config_str),
             "set -s1 score -s2 score -r1 small -r2 small -wmp false "
             "-threads %d",
             num_threads);
    Config *config = config_create_or_die(config_str);
    load_and_exec_config_or_die(config, (char *)cgp);

    Game *game = config_get_game(config);
    EndgameSolver *solver = endgame_solver_create();
    EndgameResults *results = config_get_endgame_results(config);
    ErrorStack *err = error_stack_create();

    printf("\n--- %d thread(s) ---\n", num_threads);
    fflush(stdout);

    Timer timer;
    ctimer_start(&timer);

    EndgameArgs args = {.game = game,
                        .thread_control = config_get_thread_control(config),
                        .plies = ply,
                        .tt_fraction_of_mem = tt_fraction,
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        .num_threads = num_threads,
                        .per_ply_callback = NULL,
                        .per_ply_callback_data = NULL};

    endgame_solve(solver, &args, results, err);
    double elapsed = ctimer_elapsed_seconds(&timer);
    assert(error_stack_is_empty(err));

    char *result_str = endgame_results_get_string(results, game, NULL, true);
    printf("%s", result_str);
    free(result_str);
    printf("Threads: %2d, Time: %.3fs\n", num_threads, elapsed);
    fflush(stdout);

    error_stack_destroy(err);
    endgame_solver_destroy(solver);
    config_destroy(config);
  }

  printf("\n==============================================================\n");
  printf("  Benchmark complete\n");
  printf("==============================================================\n");
}
