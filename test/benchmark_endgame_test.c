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
#include "../src/impl/cgp.h"
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

// Solve endgame with given dual-lexicon mode and return the score
static int solve_endgame_with_mode(Config *config, EndgameSolver *solver,
                                   Game *game, int ply,
                                   dual_lexicon_mode_t mode) {
  EndgameArgs args = {.game = game,
                      .thread_control = config_get_thread_control(config),
                      .plies = ply,
                      .tt_fraction_of_mem = 0.05,
                      .initial_small_move_arena_size =
                          DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                      .per_ply_callback = NULL,
                      .per_ply_callback_data = NULL,
                      .dual_lexicon_mode = mode};

  EndgameResults *results = config_get_endgame_results(config);
  ErrorStack *err = error_stack_create();

  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(solver, &args, results, err);
  assert(error_stack_is_empty(err));
  error_stack_destroy(err);

  return endgame_results_get_pvline(results)->score;
}

static void search_for_2lex_differences(Config *config, EndgameSolver *solver,
                                        int num_games, int ply,
                                        uint64_t base_seed) {
  MoveList *move_list = move_list_create(1);

  // Create the initial game (required before game_reset works)
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  int valid_endgames = 0;
  int differences_found = 0;
  const int max_attempts = num_games * 10;

  printf("Searching for positions where SHARED vs PER_PLAYER differ...\n\n");

  for (int i = 0; valid_endgames < num_games && i < max_attempts; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    valid_endgames++;

    // Solve with both modes
    int ignorant_score =
        solve_endgame_with_mode(config, solver, game, ply, DUAL_LEXICON_MODE_IGNORANT);
    int informed_score =
        solve_endgame_with_mode(config, solver, game, ply, DUAL_LEXICON_MODE_INFORMED);

    if (ignorant_score != informed_score) {
      differences_found++;
      printf("\n");
      printf("========================================\n");
      printf("DIFFERENCE FOUND! Game %d (seed %" PRIu64 ")\n", valid_endgames,
             base_seed + (uint64_t)i);
      printf("========================================\n");
      printf("IGNORANT mode score: %d\n", ignorant_score);
      printf("INFORMED mode score: %d\n", informed_score);

      // Print the game position
      StringBuilder *game_sb = string_builder_create();
      GameStringOptions *gso = game_string_options_create_default();
      string_builder_add_game(game, NULL, gso, NULL, game_sb);
      printf("%s", string_builder_peek(game_sb));
      string_builder_destroy(game_sb);
      game_string_options_destroy(gso);

      // Print CGP string for test case
      char *cgp = game_get_cgp(game, true);
      printf("\nCGP: %s\n", cgp);
      free(cgp);
      printf("========================================\n");
    } else {
      // Progress indicator
      if (valid_endgames % 10 == 0) {
        printf("Tested %d games, %d differences so far...\n", valid_endgames,
               differences_found);
      }
    }
  }

  printf("\n");
  printf("==============================================\n");
  printf("  Summary: %d/%d games had different results\n", differences_found,
         valid_endgames);
  printf("==============================================\n");

  move_list_destroy(move_list);
}

void test_benchmark_endgame(void) {
  log_set_level(LOG_WARN);

  // Use TWL98 (no QI) vs CSW24 (has QI) to find lexicon differences
  const int num_games = 30;
  const int ply = 4;
  const uint64_t base_seed = 0;

  Config *config = config_create_or_die(
      "set -l1 TWL98 -l2 CSW24 -wmp false -s1 score -s2 score -r1 small -r2 "
      "small -threads 1");

  EndgameSolver *solver = endgame_solver_create();

  printf("\n");
  printf("==============================================\n");
  printf("  2-Lexicon Endgame Test: TWL98 vs CSW24\n");
  printf("  %d games, %d-ply\n", num_games, ply);
  printf("==============================================\n");

  search_for_2lex_differences(config, solver, num_games, ply, base_seed);

  endgame_solver_destroy(solver);
  config_destroy(config);
}
