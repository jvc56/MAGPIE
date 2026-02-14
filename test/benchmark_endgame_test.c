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
#include "../src/ent/letter_distribution.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
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
                        .num_top_moves = 1,
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

static void run_benchmark_ab_full(int new_ply_setting, bool new_heuristics,
                                  int old_ply_setting, bool old_heuristics,
                                  int num_positions, uint64_t base_seed) {
  log_set_level(LOG_WARN);

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");

  EndgameSolver *solver_new = endgame_solver_create();
  EndgameSolver *solver_old = endgame_solver_create();

  EndgameArgs args_new = {.thread_control = config_get_thread_control(config),
                          .plies = new_ply_setting,
                          .tt_fraction_of_mem = 0.10,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = 6,
                          .num_top_moves = 1,
                          .use_heuristics = new_heuristics,
                          .per_ply_callback = NULL,
                          .per_ply_callback_data = NULL};

  EndgameArgs args_old = {.thread_control = config_get_thread_control(config),
                          .plies = old_ply_setting,
                          .tt_fraction_of_mem = 0.10,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = 6,
                          .num_top_moves = 1,
                          .use_heuristics = old_heuristics,
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
  printf("  Method A (new): %d-ply, heuristics=%s\n",
         new_ply, new_heuristics ? "true" : "false");
  printf("  Method B (old): %d-ply, heuristics=%s\n",
         old_ply, old_heuristics ? "true" : "false");
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
    fflush(stdout);

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

// Convenience wrapper: new=heuristics, old=classic
static void run_benchmark_ab(int new_ply, int old_ply, int num_positions,
                              uint64_t base_seed) {
  run_benchmark_ab_full(new_ply, true, old_ply, false, num_positions,
                        base_seed);
}

void test_benchmark_endgame_ab(void) { run_benchmark_ab(3, 3, 50, 42); }

void test_benchmark_endgame_ab_2v3(void) { run_benchmark_ab(2, 3, 50, 42); }

void test_benchmark_endgame_ab_3v4(void) { run_benchmark_ab(3, 4, 50, 42); }

void test_benchmark_endgame_ab_4v5(void) { run_benchmark_ab(4, 5, 10000, 1337); }

void test_benchmark_endgame_ab_4v5h(void) {
  run_benchmark_ab_full(4, true, 5, true, 50, 42);
}

// A/B benchmark loading positions from a CGP file.
// Writes per-game results (CGP, moves, scores) to output_path.
static void run_benchmark_ab_from_cgp(const char *cgp_path,
                                       const char *output_path,
                                       int new_ply, bool new_heuristics,
                                       int old_ply, bool old_heuristics,
                                       int num_threads, int max_pairs) {
  log_set_level(LOG_WARN);

  char set_cmd[256];
  snprintf(set_cmd, sizeof(set_cmd),
           "set -lex CSW21 -threads %d -s1 score -s2 score -r1 small -r2 small",
           num_threads);
  Config *config = config_create_or_die(set_cmd);

  EndgameSolver *solver_new = endgame_solver_create();
  EndgameSolver *solver_old = endgame_solver_create();

  EndgameArgs args_new = {.thread_control = config_get_thread_control(config),
                          .plies = new_ply,
                          .tt_fraction_of_mem = 0.10,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = num_threads,
                          .num_top_moves = 1,
                          .use_heuristics = new_heuristics,
                          .per_ply_callback = NULL,
                          .per_ply_callback_data = NULL};

  EndgameArgs args_old = {.thread_control = config_get_thread_control(config),
                          .plies = old_ply,
                          .tt_fraction_of_mem = 0.10,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = num_threads,
                          .num_top_moves = 1,
                          .use_heuristics = old_heuristics,
                          .per_ply_callback = NULL,
                          .per_ply_callback_data = NULL};

  EndgameResults *results = endgame_results_create();
  MoveList *move_list = move_list_create(1);

  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  FILE *cgp_file = fopen(cgp_path, "r");
  if (!cgp_file) {
    log_fatal("Cannot open %s\n", cgp_path);
  }
  FILE *out = fopen(output_path, "w");
  if (!out) {
    log_fatal("Cannot open %s for writing\n", output_path);
  }

  int total_new_spread = 0, total_old_spread = 0;
  double total_time_new = 0, total_time_old = 0;
  int num_positions = 0, new_wins = 0, old_wins = 0, ties = 0;

  printf("\n");
  printf("==============================================================\n");
  printf("  A/B Endgame Benchmark from CGP file\n");
  printf("  Source: %s\n", cgp_path);
  printf("  Output: %s\n", output_path);
  printf("  Method A (new): %d-ply, heuristics=%s\n",
         new_ply, new_heuristics ? "true" : "false");
  printf("  Method B (old): %d-ply, heuristics=%s\n",
         old_ply, old_heuristics ? "true" : "false");
  printf("  Threads: %d\n", num_threads);
  printf("==============================================================\n\n");

  fprintf(out, "# A/B: new=%d-ply(h=%s) vs old=%d-ply(h=%s), threads=%d\n",
          new_ply, new_heuristics ? "true" : "false",
          old_ply, old_heuristics ? "true" : "false", num_threads);
  fprintf(out, "# pair,new_spread,old_spread,pair_adv,time_new,time_old,cgp\n");

  char line[4096];
  while (fgets(line, sizeof(line), cgp_file) &&
         (max_pairs <= 0 || num_positions < max_pairs)) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (line[0] == '\0') continue;

    load_cgp_or_die(game, line);
    int on_turn = game_get_player_on_turn_index(game);
    num_positions++;

    Game *saved_game = game_duplicate(game);

    // Game 1: new plays as on-turn, old plays as opponent
    double time_new_1 = 0, time_old_1 = 0;
    int spread_1 = play_endgame_ab(game, solver_new, &args_new, new_ply,
                                   solver_old, &args_old, old_ply, results,
                                   move_list, &time_new_1, &time_old_1,
                                   on_turn);

    // Game 2: old plays as on-turn, new plays as opponent
    game_copy(game, saved_game);
    double time_new_2 = 0, time_old_2 = 0;
    int spread_2 = play_endgame_ab(game, solver_old, &args_old, old_ply,
                                   solver_new, &args_new, new_ply, results,
                                   move_list, &time_old_2, &time_new_2,
                                   on_turn);

    int pair_advantage = spread_1 - spread_2;
    total_new_spread += spread_1;
    total_old_spread += spread_2;
    total_time_new += time_new_1 + time_new_2;
    total_time_old += time_old_1 + time_old_2;

    if (pair_advantage > 0) new_wins++;
    else if (pair_advantage < 0) old_wins++;
    else ties++;

    fprintf(out, "%d,%+d,%+d,%+d,%.2f,%.2f,%s\n",
            num_positions, spread_1, spread_2, pair_advantage,
            time_new_1 + time_new_2, time_old_1 + time_old_2, line);
    fflush(out);

    printf("  Pair %3d: new_spread=%+4d, old_spread=%+4d, "
           "pair_adv=%+4d  (new: %.2fs, old: %.2fs)\n",
           num_positions, spread_1, spread_2, pair_advantage,
           time_new_1 + time_new_2, time_old_1 + time_old_2);
    fflush(stdout);

    game_destroy(saved_game);
  }

  fclose(cgp_file);

  int net_advantage = total_new_spread - total_old_spread;
  printf("\n==============================================================\n");
  printf("  RESULTS: %d game pairs (%d games total)\n", num_positions,
         num_positions * 2);
  printf("  New method total spread: %+d (avg %+.1f)\n", total_new_spread,
         num_positions > 0 ? (double)total_new_spread / num_positions : 0.0);
  printf("  Old method total spread: %+d (avg %+.1f)\n", total_old_spread,
         num_positions > 0 ? (double)total_old_spread / num_positions : 0.0);
  printf("  Net advantage (new): %+d (avg %+.1f per pair)\n", net_advantage,
         num_positions > 0 ? (double)net_advantage / num_positions : 0.0);
  printf("  Pair wins: new=%d, old=%d, ties=%d\n", new_wins, old_wins, ties);
  printf("  Time: new=%.2fs, old=%.2fs\n", total_time_new, total_time_old);
  printf("==============================================================\n");

  fprintf(out, "# RESULTS: %d pairs, net=%+d, W/L/T=%d/%d/%d, "
               "time_new=%.2f, time_old=%.2f\n",
          num_positions, net_advantage, new_wins, old_wins, ties,
          total_time_new, total_time_old);
  fclose(out);

  endgame_results_destroy(results);
  move_list_destroy(move_list);
  endgame_solver_destroy(solver_new);
  endgame_solver_destroy(solver_old);
  config_destroy(config);
}

// Static eval (1-ply) vs 2-ply heuristic on clean (nonstuck) positions
void test_benchmark_ab_clean_1v2h(void) {
  run_benchmark_ab_from_cgp("/tmp/clean_positions.cgp",
                             "/tmp/ab_clean_1v2h.csv",
                             2, true,   // new: 2-ply heuristic
                             1, false,  // old: 1-ply static
                             6, 1000);
}

// Play an endgame where one side uses endgame solver and the other uses
// movegen with a given sort type (greedy best move each turn).
// Returns final spread from solver player's perspective.
static int play_endgame_solver_vs_movegen(
    Game *game, EndgameSolver *solver, EndgameArgs *solver_args,
    int solver_plies, move_sort_t mg_sort_type, EndgameResults *results,
    MoveList *move_list, double *time_solver, double *time_mg,
    int solver_player_idx) {
  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    int on_turn = game_get_player_on_turn_index(game);
    Timer t;
    ctimer_start(&t);

    if (on_turn == solver_player_idx) {
      solver_args->game = game;
      solver_args->plies = solver_plies;
      ErrorStack *err = error_stack_create();
      endgame_solve(solver, solver_args, results, err);
      *time_solver += ctimer_elapsed_seconds(&t);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      const PVLine *pv = endgame_results_get_pvline(results);
      if (pv->num_moves == 0) break;
      SmallMove best = pv->moves[0];
      small_move_to_move(move_list->spare_move, &best, game_get_board(game));
      play_move(move_list->spare_move, game, NULL);
    } else {
      move_list_reset(move_list);
      const MoveGenArgs mg_args = {
          .game = game,
          .move_list = move_list,
          .move_record_type = MOVE_RECORD_BEST,
          .move_sort_type = mg_sort_type,
          .override_kwg = NULL,
          .thread_index = 0,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&mg_args);
      *time_mg += ctimer_elapsed_seconds(&t);
      if (move_list->count == 0) break;
      play_move(move_list->moves[0], game, NULL);
    }
  }
  int score_s = equity_to_int(
      player_get_score(game_get_player(game, solver_player_idx)));
  int score_e = equity_to_int(
      player_get_score(game_get_player(game, 1 - solver_player_idx)));
  return score_s - score_e;
}

// Play an endgame where both sides use movegen with different sort types.
// Returns final spread from player_a's perspective.
static int play_endgame_movegen_vs_movegen(
    Game *game, move_sort_t sort_a, move_sort_t sort_b,
    MoveList *move_list, double *time_a, double *time_b,
    int player_a_idx) {
  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    int on_turn = game_get_player_on_turn_index(game);
    move_sort_t sort = (on_turn == player_a_idx) ? sort_a : sort_b;
    double *elapsed = (on_turn == player_a_idx) ? time_a : time_b;

    Timer t;
    ctimer_start(&t);
    move_list_reset(move_list);
    const MoveGenArgs mg_args = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = sort,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&mg_args);
    *elapsed += ctimer_elapsed_seconds(&t);
    if (move_list->count == 0) break;
    play_move(move_list->moves[0], game, NULL);
  }
  int sa = equity_to_int(
      player_get_score(game_get_player(game, player_a_idx)));
  int sb = equity_to_int(
      player_get_score(game_get_player(game, 1 - player_a_idx)));
  return sa - sb;
}

// A/B benchmark: endgame solver vs movegen, loading from CGP file.
static void run_benchmark_solver_vs_movegen_from_cgp(
    const char *cgp_path, const char *output_path, int solver_ply,
    bool solver_heuristics, move_sort_t mg_sort_type, int num_threads,
    int max_pairs) {
  log_set_level(LOG_WARN);

  char set_cmd[256];
  snprintf(set_cmd, sizeof(set_cmd),
           "set -lex CSW21 -threads %d -s1 equity -s2 equity "
           "-r1 all -r2 all",
           num_threads);
  Config *config = config_create_or_die(set_cmd);

  EndgameSolver *solver = endgame_solver_create();
  EndgameArgs solver_args = {
      .thread_control = config_get_thread_control(config),
      .plies = solver_ply,
      .tt_fraction_of_mem = 0.10,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = num_threads,
      .num_top_moves = 1,
      .use_heuristics = solver_heuristics,
      .per_ply_callback = NULL,
      .per_ply_callback_data = NULL};

  EndgameResults *results = endgame_results_create();
  MoveList *move_list = move_list_create(1);

  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  FILE *cgp_file = fopen(cgp_path, "r");
  if (!cgp_file) log_fatal("Cannot open %s\n", cgp_path);
  FILE *out = fopen(output_path, "w");
  if (!out) log_fatal("Cannot open %s for writing\n", output_path);

  int total_solver_spread = 0, total_equity_spread = 0;
  double total_time_solver = 0, total_time_equity = 0;
  int num_positions = 0, solver_wins = 0, equity_wins = 0, ties = 0;

  printf("\n");
  printf("==============================================================\n");
  const char *mg_name = (mg_sort_type == MOVE_SORT_SCORE) ? "score" : "equity";
  printf("  Solver vs %s Movegen Benchmark\n", mg_name);
  printf("  Source: %s\n", cgp_path);
  printf("  Output: %s\n", output_path);
  printf("  Solver: %d-ply, heuristics=%s, threads=%d\n",
         solver_ply, solver_heuristics ? "true" : "false", num_threads);
  printf("  Movegen: greedy best-%s move\n", mg_name);
  printf("  Max pairs: %d\n", max_pairs);
  printf("==============================================================\n\n");

  fprintf(out, "# solver=%d-ply(h=%s) vs %s_movegen, threads=%d\n",
          solver_ply, solver_heuristics ? "true" : "false", mg_name,
          num_threads);
  fprintf(out, "# pair,solver_spread,equity_spread,pair_adv,"
               "time_solver,time_equity,cgp\n");

  char line[4096];
  while (fgets(line, sizeof(line), cgp_file) &&
         (max_pairs <= 0 || num_positions < max_pairs)) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (line[0] == '\0') continue;

    load_cgp_or_die(game, line);
    int on_turn = game_get_player_on_turn_index(game);
    num_positions++;

    Game *saved_game = game_duplicate(game);

    // Game 1: solver plays as on-turn
    double ts1 = 0, te1 = 0;
    int spread_1 = play_endgame_solver_vs_movegen(
        game, solver, &solver_args, solver_ply, mg_sort_type, results,
        move_list, &ts1, &te1, on_turn);

    // Game 2: movegen plays as on-turn (solver as opponent)
    game_copy(game, saved_game);
    double ts2 = 0, te2 = 0;
    int spread_2 = play_endgame_solver_vs_movegen(
        game, solver, &solver_args, solver_ply, mg_sort_type, results,
        move_list, &te2, &ts2, 1 - on_turn);
    // spread_2 is from equity's perspective; negate for solver's
    spread_2 = -spread_2;

    int pair_advantage = spread_1 - spread_2;
    total_solver_spread += spread_1;
    total_equity_spread += spread_2;
    total_time_solver += ts1 + ts2;
    total_time_equity += te1 + te2;

    if (pair_advantage > 0) solver_wins++;
    else if (pair_advantage < 0) equity_wins++;
    else ties++;

    fprintf(out, "%d,%+d,%+d,%+d,%.2f,%.2f,%s\n",
            num_positions, spread_1, spread_2, pair_advantage,
            ts1 + ts2, te1 + te2, line);
    fflush(out);

    printf("  Pair %3d: solver=%+4d, equity=%+4d, "
           "adv=%+4d  (solver: %.2fs, equity: %.2fs)\n",
           num_positions, spread_1, spread_2, pair_advantage,
           ts1 + ts2, te1 + te2);
    fflush(stdout);

    game_destroy(saved_game);
  }

  fclose(cgp_file);

  int net = total_solver_spread - total_equity_spread;
  printf("\n==============================================================\n");
  printf("  RESULTS: %d game pairs (%d games total)\n", num_positions,
         num_positions * 2);
  printf("  Solver total spread: %+d (avg %+.1f)\n", total_solver_spread,
         num_positions > 0 ? (double)total_solver_spread / num_positions : 0.0);
  printf("  Equity total spread: %+d (avg %+.1f)\n", total_equity_spread,
         num_positions > 0 ? (double)total_equity_spread / num_positions : 0.0);
  printf("  Net advantage (solver): %+d (avg %+.1f per pair)\n", net,
         num_positions > 0 ? (double)net / num_positions : 0.0);
  printf("  Pair wins: solver=%d, equity=%d, ties=%d\n",
         solver_wins, equity_wins, ties);
  printf("  Time: solver=%.2fs, equity=%.2fs\n",
         total_time_solver, total_time_equity);
  printf("==============================================================\n");

  fprintf(out, "# RESULTS: %d pairs, net=%+d, W/L/T=%d/%d/%d, "
               "time_solver=%.2f, time_equity=%.2f\n",
          num_positions, net, solver_wins, equity_wins, ties,
          total_time_solver, total_time_equity);
  fclose(out);

  endgame_results_destroy(results);
  move_list_destroy(move_list);
  endgame_solver_destroy(solver);
  config_destroy(config);
}

// Equity movegen vs 2-ply heuristic solver on clean positions
void test_benchmark_equity_vs_2ply(void) {
  run_benchmark_solver_vs_movegen_from_cgp(
      "/tmp/clean_positions.cgp", "/tmp/ab_equity_vs_2plyh.csv",
      2, true, MOVE_SORT_EQUITY, 6, 1000);
}

// Equity movegen vs 3-ply heuristic solver on clean positions
void test_benchmark_equity_vs_3ply(void) {
  run_benchmark_solver_vs_movegen_from_cgp(
      "/tmp/clean_positions.cgp", "/tmp/ab_equity_vs_3plyh.csv",
      3, true, MOVE_SORT_EQUITY, 6, 1000);
}

// Score movegen vs equity movegen on clean positions
void test_benchmark_score_vs_equity(void) {
  log_set_level(LOG_WARN);
  char set_cmd[] =
      "set -lex CSW21 -threads 1 -s1 equity -s2 equity -r1 all -r2 all";
  Config *config = config_create_or_die(set_cmd);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create(1);

  FILE *cgp_file = fopen("/tmp/clean_positions.cgp", "r");
  if (!cgp_file) log_fatal("Cannot open /tmp/clean_positions.cgp\n");
  FILE *out = fopen("/tmp/ab_score_vs_equity.csv", "w");
  if (!out) log_fatal("Cannot open output\n");

  int total_score_spread = 0, total_eq_spread = 0;
  double total_time_score = 0, total_time_eq = 0;
  int n = 0, score_wins = 0, eq_wins = 0, ties = 0;
  const int max_pairs = 1000;

  printf("\n==============================================================\n");
  printf("  Score vs Equity Movegen, max %d pairs\n", max_pairs);
  printf("==============================================================\n\n");
  fprintf(out, "# score_movegen vs equity_movegen\n");
  fprintf(out, "# pair,score_spread,equity_spread,pair_adv,time_score,time_eq,cgp\n");

  char line[4096];
  while (fgets(line, sizeof(line), cgp_file) && n < max_pairs) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    if (line[0] == '\0') continue;

    load_cgp_or_die(game, line);
    int on_turn = game_get_player_on_turn_index(game);
    n++;
    Game *saved = game_duplicate(game);

    double ts1 = 0, te1 = 0;
    int spread_1 = play_endgame_movegen_vs_movegen(
        game, MOVE_SORT_SCORE, MOVE_SORT_EQUITY, move_list,
        &ts1, &te1, on_turn);

    game_copy(game, saved);
    double ts2 = 0, te2 = 0;
    int spread_2 = play_endgame_movegen_vs_movegen(
        game, MOVE_SORT_EQUITY, MOVE_SORT_SCORE, move_list,
        &te2, &ts2, on_turn);
    spread_2 = -spread_2;

    int adv = spread_1 - spread_2;
    total_score_spread += spread_1;
    total_eq_spread += spread_2;
    total_time_score += ts1 + ts2;
    total_time_eq += te1 + te2;
    if (adv > 0) score_wins++;
    else if (adv < 0) eq_wins++;
    else ties++;

    fprintf(out, "%d,%+d,%+d,%+d,%.2f,%.2f,%s\n",
            n, spread_1, spread_2, adv, ts1 + ts2, te1 + te2, line);
    fflush(out);
    printf("  Pair %3d: score=%+4d, equity=%+4d, adv=%+4d\n",
           n, spread_1, spread_2, adv);
    fflush(stdout);
    game_destroy(saved);
  }
  fclose(cgp_file);

  int net = total_score_spread - total_eq_spread;
  printf("\n==============================================================\n");
  printf("  RESULTS: %d pairs\n", n);
  printf("  Net advantage (score): %+d (avg %+.1f per pair)\n", net,
         n > 0 ? (double)net / n : 0.0);
  printf("  Pair wins: score=%d, equity=%d, ties=%d\n",
         score_wins, eq_wins, ties);
  printf("  Time: score=%.2fs, equity=%.2fs\n",
         total_time_score, total_time_eq);
  printf("==============================================================\n");
  fprintf(out, "# RESULTS: %d pairs, net=%+d, W/L/T=%d/%d/%d\n",
          n, net, score_wins, eq_wins, ties);
  fclose(out);
  move_list_destroy(move_list);
  config_destroy(config);
}

// Score movegen vs 2-ply heuristic solver on clean positions
void test_benchmark_score_vs_2ply(void) {
  run_benchmark_solver_vs_movegen_from_cgp(
      "/tmp/clean_positions.cgp", "/tmp/ab_score_vs_2plyh.csv",
      2, true, MOVE_SORT_SCORE, 6, 1000);
}

// Score movegen vs 3-ply heuristic solver on clean positions
void test_benchmark_score_vs_3ply(void) {
  run_benchmark_solver_vs_movegen_from_cgp(
      "/tmp/clean_positions.cgp", "/tmp/ab_score_vs_3plyh.csv",
      3, true, MOVE_SORT_SCORE, 6, 1000);
}

// 4-ply heuristic vs everything: score, equity, 2ply, 3ply
void test_benchmark_4ply_tournament(void) {
  // 4ply vs score
  run_benchmark_solver_vs_movegen_from_cgp(
      "/tmp/clean_positions.cgp", "/tmp/ab_score_vs_4plyh.csv",
      4, true, MOVE_SORT_SCORE, 6, 1000);
  // 4ply vs equity
  run_benchmark_solver_vs_movegen_from_cgp(
      "/tmp/clean_positions.cgp", "/tmp/ab_equity_vs_4plyh.csv",
      4, true, MOVE_SORT_EQUITY, 6, 1000);
  // 4ply vs 2ply
  run_benchmark_ab_from_cgp("/tmp/clean_positions.cgp",
                             "/tmp/ab_clean_2v4h.csv",
                             4, true, 2, true, 6, 1000);
  // 4ply vs 3ply
  run_benchmark_ab_from_cgp("/tmp/clean_positions.cgp",
                             "/tmp/ab_clean_3v4h.csv",
                             4, true, 3, true, 6, 1000);
}

// 2-ply vs 3-ply heuristic solver on clean positions
void test_benchmark_ab_clean_2v3h(void) {
  run_benchmark_ab_from_cgp("/tmp/clean_positions.cgp",
                             "/tmp/ab_clean_2v3h.csv",
                             3, true,   // new: 3-ply heuristic
                             2, true,   // old: 2-ply heuristic
                             6, 1000);
}

// Self-play benchmark: one solver plays both sides of endgame positions.
// Useful for profiling heuristic overhead (greedy playouts, stuck-tile, etc).
static void run_benchmark_selfplay(int ply, int num_positions,
                                   uint64_t base_seed) {
  log_set_level(LOG_WARN);

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");

  EndgameSolver *solver = endgame_solver_create();

  EndgameArgs args = {.thread_control = config_get_thread_control(config),
                      .plies = ply,
                      .tt_fraction_of_mem = 0.10,
                      .initial_small_move_arena_size =
                          DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                      .num_threads = 1,
                      .num_top_moves = 1,
                      .use_heuristics = true,
                      .per_ply_callback = NULL,
                      .per_ply_callback_data = NULL};

  EndgameResults *results = endgame_results_create();
  MoveList *move_list = move_list_create(1);

  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  int valid_positions = 0;
  double total_time = 0;
  const int max_attempts = num_positions * 10;

  printf("\n");
  printf("==============================================================\n");
  printf("  Self-play Benchmark: %d positions, %d-ply, heuristics=true\n",
         num_positions, ply);
  printf("==============================================================\n\n");

  for (int i = 0; valid_positions < num_positions && i < max_attempts; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    valid_positions++;
    double game_time = 0;

    while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
      args.game = game;
      args.plies = ply;

      Timer t;
      ctimer_start(&t);
      ErrorStack *err = error_stack_create();
      endgame_solve(solver, &args, results, err);
      game_time += ctimer_elapsed_seconds(&t);
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

    total_time += game_time;
    int score0 =
        equity_to_int(player_get_score(game_get_player(game, 0)));
    int score1 =
        equity_to_int(player_get_score(game_get_player(game, 1)));
    printf("  Game %3d: %d-%d (spread %+d)  %.2fs\n", valid_positions,
           score0, score1, score0 - score1, game_time);
  }

  assert(valid_positions == num_positions);

  printf("\n==============================================================\n");
  printf("  TOTAL: %.2fs for %d games (avg %.2fs)\n", total_time,
         num_positions, total_time / num_positions);
  printf("==============================================================\n");

  endgame_results_destroy(results);
  move_list_destroy(move_list);
  endgame_solver_destroy(solver);
  config_destroy(config);
}

void test_benchmark_selfplay_4ply(void) { run_benchmark_selfplay(4, 100, 42); }

void test_benchmark_selfplay_5ply(void) { run_benchmark_selfplay(5, 100, 42); }

// Thread scaling benchmark: solve the same positions with each thread count
// in the provided array to find optimal thread count for endgame solving.
static void run_benchmark_thread_scaling(int ply, int num_positions,
                                         const int *thread_counts,
                                         int num_counts,
                                         uint64_t base_seed) {
  log_set_level(LOG_WARN);

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  // Collect endgame positions by playing games until bag is empty.
  Game **saved_games = malloc(sizeof(Game *) * (size_t)num_positions);
  int valid_positions = 0;
  const int max_attempts = num_positions * 10;

  for (int i = 0; valid_positions < num_positions && i < max_attempts; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);
    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }
    saved_games[valid_positions] = game_duplicate(game);
    valid_positions++;
  }
  assert(valid_positions == num_positions);

  printf("\n");
  printf("==============================================================\n");
  printf("  Thread Scaling: %d positions, %d-ply, heuristics=true\n",
         num_positions, ply);
  printf("==============================================================\n\n");

  for (int ti = 0; ti < num_counts; ti++) {
    int nthreads = thread_counts[ti];
    EndgameSolver *solver = endgame_solver_create();
    EndgameResults *results = endgame_results_create();

    EndgameArgs args = {.thread_control = config_get_thread_control(config),
                        .plies = ply,
                        .tt_fraction_of_mem = 0.10,
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        .num_threads = nthreads,
                        .num_top_moves = 1,
                        .use_heuristics = true,
                        .per_ply_callback = NULL,
                        .per_ply_callback_data = NULL};

    double total_time = 0;

    for (int p = 0; p < num_positions; p++) {
      game_copy(game, saved_games[p]);

      while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
        args.game = game;
        args.plies = ply;

        Timer t;
        ctimer_start(&t);
        ErrorStack *err = error_stack_create();
        endgame_solve(solver, &args, results, err);
        total_time += ctimer_elapsed_seconds(&t);
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
    }

    printf("  %d thread%s: %.2fs (avg %.3fs/game)\n", nthreads,
           nthreads == 1 ? " " : "s", total_time,
           total_time / num_positions);
    fflush(stdout);

    endgame_results_destroy(results);
    endgame_solver_destroy(solver);
  }

  printf("\n==============================================================\n");

  for (int p = 0; p < num_positions; p++) {
    game_destroy(saved_games[p]);
  }
  free(saved_games);
  move_list_destroy(move_list);
  config_destroy(config);
}

void test_benchmark_thread_scaling(void) {
  const int counts[] = {3, 6};
  run_benchmark_thread_scaling(4, 50, counts, 2, 42);
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

// Compute stuck tile fraction for a player: generate moves with
// TILES_PLAYED mode and check which rack tiles appear in no legal move.
static float compute_stuck_fraction(Game *game, MoveList *move_list,
                                    int player_idx) {
  int current_on_turn = game_get_player_on_turn_index(game);
  if (current_on_turn != player_idx) {
    game_set_player_on_turn_index(game, player_idx);
  }

  uint64_t tiles_bv = 0;
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_TILES_PLAYED,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .tiles_played_bv = &tiles_bv,
  };
  generate_moves(&args);

  if (current_on_turn != player_idx) {
    game_set_player_on_turn_index(game, current_on_turn);
  }

  const Rack *rack = player_get_rack(game_get_player(game, player_idx));
  const LetterDistribution *ld = game_get_ld(game);
  int total = 0, stuck = 0;
  int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    int count = rack_get_letter(rack, ml);
    if (count > 0) {
      total += count;
      if (!(tiles_bv & ((uint64_t)1 << ml))) {
        stuck += count;
      }
    }
  }
  if (total == 0) return 0.0f;
  return (float)stuck / (float)total;
}

// Survey 1000 endgame positions for stuck tile frequency.
// Buckets: 0%, 1-19%, 20-39%, 40-59%, 60-79%, 80-99%, 100%
void test_stuck_tile_survey(void) {
  log_set_level(LOG_FATAL);

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  const int num_positions = 10000;
  const uint64_t base_seed = 42;
  const int max_attempts = num_positions * 10;

  // Buckets: 0%, 1-19%, 20-39%, 40-59%, 60-79%, 80-99%, 100%
  const int NUM_BUCKETS = 7;
  // Per-depth buckets (depth 0 and 1) and counters
  int buckets[2][7] = {{0}};
  int any_stuck_at_depth[2] = {0};
  int ever_stuck = 0;
  int valid_positions = 0;

  // Write CGPs to files
  FILE *cgp_stuck = fopen("/tmp/stuck_tile_positions.cgp", "w");
  FILE *cgp_clean = fopen("/tmp/clean_positions.cgp", "w");
  assert(cgp_stuck);
  assert(cgp_clean);
  int stuck_count = 0;
  int clean_count = 0;

  for (int i = 0; valid_positions < num_positions && i < max_attempts; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    valid_positions++;

    // Save root position
    Game *saved = game_duplicate(game);
    bool found_stuck = false;

    for (int d = 0; d <= 1; d++) {
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        break;
      }

      float frac_p0 = compute_stuck_fraction(game, move_list, 0);
      float frac_p1 = compute_stuck_fraction(game, move_list, 1);
      float max_frac = frac_p0 > frac_p1 ? frac_p0 : frac_p1;

      if (max_frac > 0.0f) {
        any_stuck_at_depth[d]++;
        found_stuck = true;
      }

      int bucket;
      if (max_frac == 0.0f) bucket = 0;
      else if (max_frac < 0.20f) bucket = 1;
      else if (max_frac < 0.40f) bucket = 2;
      else if (max_frac < 0.60f) bucket = 3;
      else if (max_frac < 0.80f) bucket = 4;
      else if (max_frac < 1.00f) bucket = 5;
      else bucket = 6;
      buckets[d][bucket]++;

      // Play greedy best move to advance to depth 1
      if (d == 0) {
        move_list_reset(move_list);
        const MoveGenArgs mg_args = {
            .game = game,
            .move_list = move_list,
            .move_record_type = MOVE_RECORD_BEST,
            .move_sort_type = MOVE_SORT_EQUITY,
            .override_kwg = NULL,
            .thread_index = 0,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        };
        generate_moves(&mg_args);
        if (move_list->count == 0) break;
        play_move(move_list->moves[0], game, NULL);
      }
    }

    char *cgp = game_get_cgp(saved, true);
    if (found_stuck) {
      ever_stuck++;
      fprintf(cgp_stuck, "%s\n", cgp);
      stuck_count++;
    } else {
      fprintf(cgp_clean, "%s\n", cgp);
      clean_count++;
    }
    free(cgp);

    game_copy(game, saved);
    game_destroy(saved);
  }

  fclose(cgp_stuck);
  fclose(cgp_clean);
  assert(valid_positions == num_positions);

  printf("\n");
  printf("==============================================================\n");
  printf("  Stuck Tile Survey: %d positions, depths 0-1 (seed=%llu)\n",
         num_positions, (unsigned long long)base_seed);
  printf("==============================================================\n\n");
  printf("  Positions with stuck tiles at depth 0 or 1: %d/%d (%.1f%%)\n",
         ever_stuck, num_positions,
         100.0 * ever_stuck / num_positions);
  printf("  Stuck:  /tmp/stuck_tile_positions.cgp (%d positions)\n",
         stuck_count);
  printf("  Clean:  /tmp/clean_positions.cgp (%d positions)\n\n",
         clean_count);

  const char *labels[] = {"    0%", " 1-19%", "20-39%", "40-59%",
                           "60-79%", "80-99%", "  100%"};

  for (int d = 0; d <= 1; d++) {
    int reached = 0;
    for (int b = 0; b < NUM_BUCKETS; b++) reached += buckets[d][b];
    if (reached == 0) break;

    printf("  Depth %d: %d/%d have stuck tiles (%.1f%%)\n",
           d, any_stuck_at_depth[d], reached,
           100.0 * any_stuck_at_depth[d] / reached);
    for (int b = 0; b < NUM_BUCKETS; b++) {
      if (buckets[d][b] > 0) {
        printf("    %s: %4d (%5.1f%%)\n", labels[b], buckets[d][b],
               100.0 * buckets[d][b] / reached);
      }
    }
    printf("\n");
  }
  printf("==============================================================\n");
  fflush(stdout);

  move_list_destroy(move_list);
  config_destroy(config);
}

// Generate random endgame positions until we collect target_stuck stuck-tile
// positions, then report which letters get stuck most often.
// Checks both players at depth 0 and depth 1 (after greedy move).
void test_stuck_letter_frequency(void) {
  log_set_level(LOG_FATAL);

  const char *lex = "CEL22";

  // Generate CEL22 data files if they don't exist
  {
    Config *setup = config_create_or_die(
        "set -lex TWL06 -wmp false -s1 score -s2 score -numplays 1");
    load_and_exec_config_or_die(setup, "convert text2kwg CEL22");
    load_and_exec_config_or_die(setup, "convert text2wordmap CEL22");
    config_destroy(setup);

    // Copy TWL06.klv2 as CEL22.klv2 if needed
    const char *src = "data/lexica/TWL06.klv2";
    const char *dst = "data/lexica/CEL22.klv2";
    FILE *check = fopen(dst, "r");
    if (!check) {
      FILE *in = fopen(src, "rb");
      assert(in);
      FILE *out = fopen(dst, "wb");
      assert(out);
      char buf[8192];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
      }
      fclose(in);
      fclose(out);
    } else {
      fclose(check);
    }
  }

  char lex_cmd[256];
  snprintf(lex_cmd, sizeof(lex_cmd),
           "set -lex %s -threads 1 -s1 score -s2 score -r1 small -r2 small",
           lex);
  Config *config = config_create_or_die(lex_cmd);
  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  const int target_stuck = 1000;
  const uint64_t base_seed = 42;

  // Per-letter stuck counts and rack appearance counts
  int stuck_counts[MAX_ALPHABET_SIZE] = {0};
  int rack_counts[MAX_ALPHABET_SIZE] = {0};
  int stuck_found = 0;
  int games_tried = 0;
  int total_stuck_tiles = 0;

  for (int i = 0; stuck_found < target_stuck; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }
    games_tried++;

    Game *saved = game_duplicate(game);
    bool found_stuck = false;

    for (int d = 0; d <= 1; d++) {
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) break;

      for (int p = 0; p < 2; p++) {
        const Rack *rack = player_get_rack(game_get_player(game, p));
        if (rack_get_total_letters(rack) == 0) continue;

        int current_on_turn = game_get_player_on_turn_index(game);
        if (current_on_turn != p) {
          game_set_player_on_turn_index(game, p);
        }

        uint64_t tiles_bv = 0;
        const MoveGenArgs tp_args = {
            .game = game,
            .move_list = move_list,
            .move_record_type = MOVE_RECORD_TILES_PLAYED,
            .move_sort_type = MOVE_SORT_SCORE,
            .override_kwg = NULL,
            .thread_index = 0,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
            .tiles_played_bv = &tiles_bv,
        };
        generate_moves(&tp_args);

        if (current_on_turn != p) {
          game_set_player_on_turn_index(game, current_on_turn);
        }

        for (int ml = 0; ml < ld_size; ml++) {
          int count = rack_get_letter(rack, ml);
          if (count > 0) {
            rack_counts[ml] += count;
            if (!(tiles_bv & ((uint64_t)1 << ml))) {
              stuck_counts[ml] += count;
              total_stuck_tiles += count;
              found_stuck = true;
            }
          }
        }
      }

      // Play greedy move to advance to depth 1
      if (d == 0) {
        move_list_reset(move_list);
        const MoveGenArgs mg_args = {
            .game = game,
            .move_list = move_list,
            .move_record_type = MOVE_RECORD_BEST,
            .move_sort_type = MOVE_SORT_EQUITY,
            .override_kwg = NULL,
            .thread_index = 0,
            .eq_margin_movegen = 0,
            .target_equity = EQUITY_MAX_VALUE,
            .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        };
        generate_moves(&mg_args);
        if (move_list->count == 0) break;
        play_move(move_list->moves[0], game, NULL);
      }
    }

    if (found_stuck) stuck_found++;
    game_copy(game, saved);
    game_destroy(saved);
  }

  // Sort by stuck count descending
  int order[MAX_ALPHABET_SIZE];
  for (int i = 0; i < ld_size; i++) order[i] = i;
  for (int i = 0; i < ld_size - 1; i++) {
    for (int j = i + 1; j < ld_size; j++) {
      if (stuck_counts[order[j]] > stuck_counts[order[i]]) {
        int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
      }
    }
  }

  printf("\n");
  printf("==============================================================\n");
  printf("  Stuck Letter Frequency: %d stuck positions from %d games\n",
         stuck_found, games_tried);
  printf("  (depths 0-1, both players, seed=%llu)\n",
         (unsigned long long)base_seed);
  printf("==============================================================\n\n");
  printf("  %-8s %6s  %6s  %8s\n", "Letter", "Stuck", "OnRack", "StuckRate");
  printf("  %-8s %6s  %6s  %8s\n", "------", "-----", "------", "--------");

  for (int i = 0; i < ld_size; i++) {
    int ml = order[i];
    if (stuck_counts[ml] == 0) break;
    char *letter = ld_ml_to_hl(ld, ml);
    float rate = rack_counts[ml] > 0
                     ? 100.0f * stuck_counts[ml] / rack_counts[ml]
                     : 0.0f;
    printf("  %-8s %6d  %6d  %7.1f%%\n", letter, stuck_counts[ml],
           rack_counts[ml], (double)rate);
    free(letter);
  }

  printf("\n  Total stuck tile instances: %d\n", total_stuck_tiles);
  printf("  Stuck rate: %.1f%% of endgame positions\n",
         100.0 * stuck_found / games_tried);
  printf("==============================================================\n");
  fflush(stdout);

  move_list_destroy(move_list);
  config_destroy(config);
}

// Tournament on stuck-tile positions: equity, 2-ply, 3-ply
void test_benchmark_stuck_tournament(void) {
  const char *cgp = "/tmp/stuck_tile_positions.cgp";
  // equity vs 2-ply
  run_benchmark_solver_vs_movegen_from_cgp(
      cgp, "/tmp/ab_stuck_equity_vs_2plyh.csv",
      2, true, MOVE_SORT_EQUITY, 6, 0);
  // equity vs 3-ply
  run_benchmark_solver_vs_movegen_from_cgp(
      cgp, "/tmp/ab_stuck_equity_vs_3plyh.csv",
      3, true, MOVE_SORT_EQUITY, 6, 0);
  // 2-ply vs 3-ply
  run_benchmark_ab_from_cgp(cgp, "/tmp/ab_stuck_2v3h.csv",
                             3, true, 2, true, 6, 0);
}

// 4-ply and 5-ply round robin on stuck positions, then 5-ply on clean
void test_benchmark_deep_tournament(void) {
  // Use no-monster file (line 161 removed — that position takes 3+ hours at
  // 5-ply). 4-ply stuck results already captured from prior run with full file.
  const char *stuck = "/tmp/stuck_tile_positions_no_monster.cgp";
  const char *clean = "/tmp/clean_positions.cgp";

  // --- 5-ply vs remaining opponents on stuck positions ---
  // (5-ply vs equity and 5-ply vs 2-ply already done from prior run)
  // Use 2 threads to avoid OOM on 8GB machine
  run_benchmark_ab_from_cgp(stuck, "/tmp/ab_stuck_3v5h.csv",
                             5, true, 3, true, 2, 0);
  run_benchmark_ab_from_cgp(stuck, "/tmp/ab_stuck_4v5h.csv",
                             5, true, 4, true, 2, 0);

  // --- 5-ply vs everything on clean positions ---
  // Clean positions are easier, can use more threads
  run_benchmark_solver_vs_movegen_from_cgp(
      clean, "/tmp/ab_clean_equity_vs_5plyh.csv",
      5, true, MOVE_SORT_EQUITY, 6, 1000);
  run_benchmark_ab_from_cgp(clean, "/tmp/ab_clean_2v5h.csv",
                             5, true, 2, true, 6, 1000);
  run_benchmark_ab_from_cgp(clean, "/tmp/ab_clean_3v5h.csv",
                             5, true, 3, true, 6, 1000);
  run_benchmark_ab_from_cgp(clean, "/tmp/ab_clean_4v5h.csv",
                             5, true, 4, true, 6, 1000);
}
