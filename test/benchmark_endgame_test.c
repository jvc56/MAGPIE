#include "benchmark_endgame_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/game_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
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
                        .use_heuristics = true,
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

// Play out an endgame position move-by-move with A/B solver settings.
// solver_a is used when player_a_idx is on turn, solver_b otherwise.
// Returns final spread from player_a_idx's perspective.
static int play_endgame_ab(Game *game, EndgameSolver *solver_a,
                           EndgameArgs *args_a, int max_plies_a,
                           EndgameSolver *solver_b, EndgameArgs *args_b,
                           int max_plies_b, EndgameResults *results,
                           MoveList *move_list, double *time_a, double *time_b,
                           int player_a_idx) {
  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    int on_turn = game_get_player_on_turn_index(game);
    EndgameSolver *solver = (on_turn == player_a_idx) ? solver_a : solver_b;
    EndgameArgs *args = (on_turn == player_a_idx) ? args_a : args_b;
    int max_plies = (on_turn == player_a_idx) ? max_plies_a : max_plies_b;
    double *elapsed = (on_turn == player_a_idx) ? time_a : time_b;

    args->game = game;
    args->plies = max_plies;

    Timer t;
    ctimer_start(&t);
    ErrorStack *err = error_stack_create();
    endgame_solve(solver, args, results, err);
    *elapsed += ctimer_elapsed_seconds(&t);
    assert(error_stack_is_empty(err));
    error_stack_destroy(err);

    const PVLine *pv = endgame_results_get_pvline(results);
    if (pv->num_moves == 0)
      break;

    SmallMove best = pv->moves[0];
    small_move_to_move(move_list->spare_move, &best,
                       game_get_board(game));
    play_move(move_list->spare_move, game, NULL);
  }
  int score_a =
      equity_to_int(player_get_score(game_get_player(game, player_a_idx)));
  int score_b = equity_to_int(
      player_get_score(game_get_player(game, 1 - player_a_idx)));
  return score_a - score_b;
}

static void run_benchmark_ab(int new_ply_setting, int old_ply_setting,
                             int num_positions, uint64_t base_seed) {
  log_set_level(LOG_WARN);

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");

  EndgameSolver *solver_new = endgame_solver_create();
  EndgameSolver *solver_old = endgame_solver_create();

  EndgameArgs args_new = {.thread_control = config_get_thread_control(config),
                          .plies = new_ply_setting,
                          .tt_fraction_of_mem = 0.25,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = 1,
                          .use_heuristics = true,
                          .per_ply_callback = NULL,
                          .per_ply_callback_data = NULL};

  EndgameArgs args_old = {.thread_control = config_get_thread_control(config),
                          .plies = old_ply_setting,
                          .tt_fraction_of_mem = 0.25,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = 1,
                          .use_heuristics = false,
                          .per_ply_callback = NULL,
                          .per_ply_callback_data = NULL};

  EndgameResults *results = endgame_results_create();
  MoveList *move_list = move_list_create(1);

  // Create the initial game
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  int total_new_spread = 0;
  int total_old_spread = 0;
  double total_time_new = 0;
  double total_time_old = 0;
  int valid_positions = 0;
  int new_wins = 0;
  int old_wins = 0;
  int ties = 0;
  const int max_attempts = num_positions * 10;

  printf("\n");
  printf("==============================================================\n");
  int new_ply = args_new.plies;
  int old_ply = args_old.plies;
  printf("  A/B Endgame Benchmark: %d positions, game pairs\n",
         num_positions);
  printf("  Method A (new): %d-ply, heuristics=true  (stuck-tile + greedy leaf)\n",
         new_ply);
  printf("  Method B (old): %d-ply, heuristics=false (classic evaluation)\n",
         old_ply);
  printf("==============================================================\n\n");

  for (int i = 0; valid_positions < num_positions && i < max_attempts; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    int on_turn = game_get_player_on_turn_index(game);
    valid_positions++;

    // Save position for replay
    Game *saved_game = game_duplicate(game);

    // Game 1: new method plays as on-turn player, old plays as opponent
    double time_new_1 = 0, time_old_1 = 0;
    int spread_1 = play_endgame_ab(game, solver_new, &args_new, new_ply,
                                   solver_old, &args_old, old_ply, results,
                                   move_list, &time_new_1, &time_old_1,
                                   on_turn);

    // Game 2: old method plays as on-turn player, new plays as opponent
    game_copy(game, saved_game); // restore position
    double time_new_2 = 0, time_old_2 = 0;
    int spread_2 = play_endgame_ab(game, solver_old, &args_old, old_ply,
                                   solver_new, &args_new, new_ply, results,
                                   move_list, &time_old_2, &time_new_2,
                                   on_turn);

    // spread_1 is from new's perspective, spread_2 is from old's perspective
    int pair_advantage = spread_1 - spread_2;
    total_new_spread += spread_1;
    total_old_spread += spread_2;
    total_time_new += time_new_1 + time_new_2;
    total_time_old += time_old_1 + time_old_2;

    if (pair_advantage > 0)
      new_wins++;
    else if (pair_advantage < 0)
      old_wins++;
    else
      ties++;

    printf("  Pair %3d: new_spread=%+4d, old_spread=%+4d, "
           "pair_adv=%+4d  (new: %.2fs, old: %.2fs)\n",
           valid_positions, spread_1, spread_2, pair_advantage,
           time_new_1 + time_new_2, time_old_1 + time_old_2);

    game_destroy(saved_game);
  }

  assert(valid_positions == num_positions);

  int net_advantage = total_new_spread - total_old_spread;
  printf("\n==============================================================\n");
  printf("  RESULTS: %d game pairs (%d games total)\n", num_positions,
         num_positions * 2);
  printf("  New method total spread: %+d (avg %+.1f)\n", total_new_spread,
         (double)total_new_spread / num_positions);
  printf("  Old method total spread: %+d (avg %+.1f)\n", total_old_spread,
         (double)total_old_spread / num_positions);
  printf("  Net advantage (new): %+d (avg %+.1f per pair)\n", net_advantage,
         (double)net_advantage / num_positions);
  printf("  Pair wins: new=%d, old=%d, ties=%d\n", new_wins, old_wins, ties);
  printf("  Time: new=%.2fs, old=%.2fs\n", total_time_new, total_time_old);
  printf("==============================================================\n");

  endgame_results_destroy(results);
  move_list_destroy(move_list);
  endgame_solver_destroy(solver_new);
  endgame_solver_destroy(solver_old);
  config_destroy(config);
}

void test_benchmark_endgame_ab(void) { run_benchmark_ab(3, 3, 50, 42); }

void test_benchmark_endgame_ab_2v3(void) { run_benchmark_ab(2, 3, 50, 42); }

void test_benchmark_endgame_ab_3v4(void) { run_benchmark_ab(3, 4, 50, 42); }

void test_benchmark_endgame_ab_4v5(void) { run_benchmark_ab(4, 6, 25, 2000); }

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
