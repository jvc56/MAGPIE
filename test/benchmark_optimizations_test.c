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
  bool lmr;
  bool aspiration;
  double total_time;
  int agreements;  // agreements with baseline
  int total_games;
} ModeStats;

// Solve endgame with specific optimization settings
static int32_t solve_with_settings(Config *config, EndgameSolver *solver,
                                   Game *game, int ply, bool lmr,
                                   bool aspiration, double *elapsed_out) {
  // Configure optimizations via the solver's internal state
  // We need to access the solver's fields - for now we'll use the external API
  // The solver gets reset in endgame_solve, so we need a different approach

  Timer timer;
  ctimer_start(&timer);

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

  endgame_solver_set_lmr(solver, lmr);
  endgame_solver_set_aspiration(solver, aspiration);

  endgame_solve(solver, &args, results, err);

  *elapsed_out = ctimer_elapsed_seconds(&timer);
  assert(error_stack_is_empty(err));
  error_stack_destroy(err);

  const PVLine *pv = endgame_results_get_pvline(results);
  return pv->score;
}

void test_benchmark_optimizations(void) {
  log_set_level(LOG_FATAL);  // Suppress warnings for cleaner output

  const int num_games = 100;
  const int ply = 4;
  const uint64_t base_seed = 12345;

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  EndgameSolver *solver = endgame_solver_create();
  MoveList *move_list = move_list_create(1);

  // Create initial game
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  ModeStats modes[4] = {
      {"baseline (no opts)", false, false, 0, 0, 0},
      {"LMR only", true, false, 0, 0, 0},
      {"aspiration only", false, true, 0, 0, 0},
      {"LMR + aspiration", true, true, 0, 0, 0},
  };

  printf("\n");
  printf("==============================================\n");
  printf("  Optimization Comparison: %d games, %d-ply\n", num_games, ply);
  printf("==============================================\n\n");

  int valid_endgames = 0;
  const int max_attempts = num_games * 10;

  for (int i = 0; valid_endgames < num_games && i < max_attempts; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    printf("Game %d: ", valid_endgames + 1);
    fflush(stdout);

    int32_t baseline_value = 0;

    // Run all 4 modes on this position
    for (int m = 0; m < 4; m++) {
      // Reset game to same position before each solve
      game_reset(game);
      game_seed(game, base_seed + (uint64_t)i);
      draw_starting_racks(game);
      play_until_bag_empty(game, move_list);

      double elapsed;
      int32_t value = solve_with_settings(config, solver, game, ply,
                                          modes[m].lmr, modes[m].aspiration,
                                          &elapsed);

      modes[m].total_time += elapsed;
      modes[m].total_games++;

      if (m == 0) {
        baseline_value = value;
        modes[m].agreements++;  // baseline always agrees with itself
      } else {
        if (value == baseline_value) {
          modes[m].agreements++;
        }
      }
    }

    printf("baseline=%d\n", baseline_value);
    valid_endgames++;
  }

  printf("\n==============================================\n");
  printf("  RESULTS\n");
  printf("==============================================\n\n");

  printf("%-25s %10s %10s %12s\n", "Mode", "Agree", "Time/game", "Total time");
  printf("%-25s %10s %10s %12s\n", "----", "-----", "---------", "----------");

  for (int m = 0; m < 4; m++) {
    double avg_time = modes[m].total_time / modes[m].total_games;
    printf("%-25s %7d/%d %9.3fs %11.1fs\n",
           modes[m].name,
           modes[m].agreements, modes[m].total_games,
           avg_time, modes[m].total_time);
  }

  printf("\n");
  printf("Speedups vs baseline:\n");
  double baseline_avg = modes[0].total_time / modes[0].total_games;
  for (int m = 1; m < 4; m++) {
    double avg_time = modes[m].total_time / modes[m].total_games;
    double speedup = baseline_avg / avg_time;
    printf("  %s: %.2fx faster, %d/%d agreement\n",
           modes[m].name, speedup, modes[m].agreements, modes[m].total_games);
  }

  move_list_destroy(move_list);
  endgame_solver_destroy(solver);
  config_destroy(config);
}
