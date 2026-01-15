#include "benchmark_endgame_test.h"

#include "../src/def/game_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/exec.h"
#include "../src/impl/gameplay.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern bool move_gen_use_optimized_record_all_small;

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

// Per-ply callback to print PV during iterative deepening
static void print_pv_callback(int depth, int32_t value, const PVLine *pv_line,
                              const Game *game, void *user_data) {
  double *start_time = (double *)user_data;
  double elapsed = get_time_sec() - *start_time;

  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(sb, "  depth %d: value=%d, time=%.3fs, pv=",
                                      depth, value, elapsed);

  // Format each move in the PV
  Game *game_copy = game_duplicate(game);
  const Board *board = game_get_board(game_copy);
  const LetterDistribution *ld = game_get_ld(game_copy);
  Move move;

  for (int i = 0; i < pv_line->num_moves; i++) {
    small_move_to_move(&move, &(pv_line->moves[i]), board);
    string_builder_add_move(sb, board, &move, ld, true);
    if (i < pv_line->num_moves - 1) {
      string_builder_add_string(sb, " ");
    }
    // Play the move to update the board for the next move
    play_move(&move, game_copy, NULL);
  }

  printf("%s\n", string_builder_peek(sb));
  string_builder_destroy(sb);
  game_destroy(game_copy);
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

typedef struct BenchmarkStats {
  double total_time;
  double min_time;
  double max_time;
  int games_solved;
} BenchmarkStats;

static void run_endgames_with_pv(Config *config, EndgameSolver *solver,
                                  int num_games, int ply, int num_threads,
                                  uint64_t base_seed, BenchmarkStats *stats) {
  MoveList *move_list = move_list_create(1);

  // Create the initial game (required before game_reset works)
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  int valid_endgames = 0;
  stats->total_time = 0;
  stats->min_time = 1e9;
  stats->max_time = 0;
  stats->games_solved = 0;

  for (int i = 0; valid_endgames < num_games; i++) {
    // Reset game directly (avoids spurious output from "new" command)
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    // Print the endgame position
    printf("\n--- Game %d (seed %llu) ---\n", valid_endgames + 1,
           (unsigned long long)(base_seed + (uint64_t)i));
    StringBuilder *game_sb = string_builder_create();
    GameStringOptions *gso = game_string_options_create_default();
    string_builder_add_game(game, NULL, gso, NULL, game_sb);
    printf("%s", string_builder_peek(game_sb));
    string_builder_destroy(game_sb);
    game_string_options_destroy(gso);

    double start_time = get_time_sec();
    EndgameArgs args = {.game = game,
                        .thread_control = config_get_thread_control(config),
                        .plies = ply,
                        .tt_fraction_of_mem = 0.5,  // 50% of memory
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        .num_threads = num_threads,
                        .per_ply_callback = print_pv_callback,
                        .per_ply_callback_data = &start_time};
    EndgameResults *results = config_get_endgame_results(config);
    ErrorStack *err = error_stack_create();

    printf("Solving %d-ply endgame with %d threads...\n", ply, num_threads);
    endgame_solve(solver, &args, results, err);
    assert(error_stack_is_empty(err));

    double elapsed = get_time_sec() - start_time;
    stats->total_time += elapsed;
    if (elapsed < stats->min_time) stats->min_time = elapsed;
    if (elapsed > stats->max_time) stats->max_time = elapsed;

    valid_endgames++;
    stats->games_solved = valid_endgames;
    error_stack_destroy(err);
  }

  move_list_destroy(move_list);
}

void test_benchmark_endgame(void) {
  log_set_level(LOG_WARN);  // Allow warnings to show diagnostics

  const int num_games = 100;  // Full benchmark
  const int ply = 8;          // 8-ply search
  const int num_threads = 10;
  const uint64_t base_seed = 0;

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  EndgameSolver *solver = endgame_solver_create();

  printf("\n");
  printf("==============================================\n");
  printf("  Endgame Benchmark: %d games, %d-ply, %d threads\n", num_games, ply,
         num_threads);
  printf("==============================================\n");

  BenchmarkStats stats;
  run_endgames_with_pv(config, solver, num_games, ply, num_threads, base_seed,
                       &stats);

  printf("\n");
  printf("==============================================\n");
  printf("  BENCHMARK RESULTS\n");
  printf("==============================================\n");
  printf("  Games solved:      %d\n", stats.games_solved);
  printf("  Ply depth:         %d\n", ply);
  printf("  Threads:           %d\n", num_threads);
  printf("  Total time:        %.3f s\n", stats.total_time);
  printf("  Average time:      %.3f s/game\n",
         stats.total_time / stats.games_solved);
  printf("  Min time:          %.3f s\n", stats.min_time);
  printf("  Max time:          %.3f s\n", stats.max_time);
  printf("==============================================\n\n");

  endgame_solver_destroy(solver);
  config_destroy(config);
}

// Functions for movegen benchmark comparison
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
                        .tt_fraction_of_mem = 0.5,
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
                        .tt_fraction_of_mem = 0.5,
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
