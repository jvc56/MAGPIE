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

  (void)fflush(stdout);
  (void)dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
}

// Play moves until the bag is empty, returning true if we get a valid endgame
static bool play_until_bag_empty(Game *game, MoveList *move_list) {
  while (bag_get_letters(game_get_bag(game)) > 0) {
    const Move *move = get_top_equity_move(game, 0, move_list);
    play_move(move, game, NULL);
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      return false;
    }
  }
  const Rack *rack0 = player_get_rack(game_get_player(game, 0));
  const Rack *rack1 = player_get_rack(game_get_player(game, 1));
  return !rack_is_empty(rack0) && !rack_is_empty(rack1);
}

typedef struct {
  const char *name;
  bool aspiration;
  double total_time;
  int total_games;
  int32_t *values;  // Store values per game for comparison
} ModeStats;

void test_benchmark_optimizations(void) {
  log_set_level(LOG_FATAL);  // Suppress warnings for cleaner output

  const int num_games = 20;
  const int ply = 6;
  const uint64_t base_seed = 12345;

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  MoveList *move_list = move_list_create(1);

  // Create initial game
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  ModeStats modes[2] = {
      {"baseline (no aspiration)", false, 0, 0, NULL},
      {"aspiration windows", true, 0, 0, NULL},
  };

  // Allocate value arrays
  for (int m = 0; m < 2; m++) {
    modes[m].values = malloc(num_games * sizeof(int32_t));
  }

  printf("\n");
  printf("==============================================\n");
  printf("  Aspiration Window Benchmark: %d games, %d-ply\n", num_games, ply);
  printf("  (One fresh TT per mode, reused across positions)\n");
  printf("==============================================\n\n");

  // First, find all valid endgame seeds
  uint64_t *valid_seeds = malloc(num_games * sizeof(uint64_t));
  int valid_count = 0;
  const int max_attempts = num_games * 10;

  printf("Finding %d valid endgame positions...\n", num_games);
  for (int i = 0; valid_count < num_games && i < max_attempts; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (play_until_bag_empty(game, move_list)) {
      valid_seeds[valid_count++] = base_seed + (uint64_t)i;
    }
  }
  printf("Found %d valid endgames.\n\n", valid_count);

  // Run each mode on ALL positions before moving to next mode
  for (int m = 0; m < 2; m++) {
    printf("Running %s on %d positions...\n", modes[m].name, valid_count);
    fflush(stdout);

    // Create fresh solver for this mode (fresh TT)
    EndgameSolver *solver = endgame_solver_create();
    endgame_solver_set_aspiration(solver, modes[m].aspiration);

    Timer mode_timer;
    ctimer_start(&mode_timer);

    for (int g = 0; g < valid_count; g++) {
      // Setup position
      game_reset(game);
      game_seed(game, valid_seeds[g]);
      draw_starting_racks(game);
      play_until_bag_empty(game, move_list);

      EndgameArgs args = {.game = game,
                          .thread_control = config_get_thread_control(config),
                          .plies = ply,
                          .tt_fraction_of_mem = 0.05,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .per_ply_callback = NULL,
                          .per_ply_callback_data = NULL};
      EndgameResults *results = config_get_endgame_results(config);
      ErrorStack *err = error_stack_create();

      endgame_solve(solver, &args, results, err);

      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      const PVLine *pv = endgame_results_get_pvline(results);
      modes[m].values[g] = pv->score;
      modes[m].total_games++;
    }

    modes[m].total_time = ctimer_elapsed_seconds(&mode_timer);
    endgame_solver_destroy(solver);

    printf("  Completed in %.1fs (%.3fs/game)\n", modes[m].total_time,
           modes[m].total_time / modes[m].total_games);
  }

  // Compare results
  printf("\n==============================================\n");
  printf("  RESULTS\n");
  printf("==============================================\n\n");

  // Count agreements with baseline
  int agreements = 0;
  for (int g = 0; g < valid_count; g++) {
    if (modes[1].values[g] == modes[0].values[g]) {
      agreements++;
    }
  }

  printf("%-30s %10s %10s %12s\n", "Mode", "Agree", "Time/game", "Total time");
  printf("%-30s %10s %10s %12s\n", "----", "-----", "---------", "----------");

  for (int m = 0; m < 2; m++) {
    double avg_time = modes[m].total_time / modes[m].total_games;
    int agree = (m == 0) ? valid_count : agreements;
    printf("%-30s %7d/%d %9.3fs %11.1fs\n", modes[m].name, agree,
           modes[m].total_games, avg_time, modes[m].total_time);
  }

  printf("\n");
  double baseline_avg = modes[0].total_time / modes[0].total_games;
  double asp_avg = modes[1].total_time / modes[1].total_games;
  double speedup = baseline_avg / asp_avg;
  printf("Aspiration windows: %.2fx %s, %d/%d agreement\n",
         speedup >= 1.0 ? speedup : 1.0 / speedup,
         speedup >= 1.0 ? "faster" : "slower", agreements, valid_count);

  // Cleanup
  for (int m = 0; m < 2; m++) {
    free(modes[m].values);
  }
  free(valid_seeds);
  move_list_destroy(move_list);
  config_destroy(config);
}
