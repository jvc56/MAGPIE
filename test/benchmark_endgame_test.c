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

static void run_endgames_with_pv(Config *config, EndgameSolver *solver,
                                  int num_games, int ply, int num_threads,
                                  uint64_t base_seed) {
  MoveList *move_list = move_list_create(1);

  // Create the initial game (required before game_reset works)
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  int valid_endgames = 0;

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

    double game_start = get_time_sec();
    EndgameArgs args = {.game = game,
                        .thread_control = config_get_thread_control(config),
                        .plies = ply,
                        .tt_fraction_of_mem = 0.1,  // 10% to reduce TT alloc time
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        .num_threads = num_threads,
                        .per_ply_callback = NULL,  // Disable callback
                        .per_ply_callback_data = NULL};
    EndgameResults *results = config_get_endgame_results(config);
    ErrorStack *err = error_stack_create();

    printf("Solving %d-ply endgame with %d threads...\n", ply, num_threads);
    endgame_solve(solver, &args, results, err);
    double game_elapsed = get_time_sec() - game_start;
    printf("  Game solved in %.3fs\n", game_elapsed);
    assert(error_stack_is_empty(err));

    valid_endgames++;
    error_stack_destroy(err);
  }

  move_list_destroy(move_list);
}

void test_benchmark_endgame(void) {
  log_set_level(LOG_WARN);  // Allow warnings to show diagnostics

  const int num_games = 5;
  const int ply = 8;  // Balance depth vs time
  const uint64_t base_seed = 42;  // Fixed seed for reproducibility

  // Thread counts to benchmark
  int thread_counts[] = {1, 12};
  int num_configs = sizeof(thread_counts) / sizeof(thread_counts[0]);

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  printf("\n");
  printf("==============================================\n");
  printf("  ABDADA Endgame Benchmark: %d games, %d-ply\n", num_games, ply);
  printf("==============================================\n");

  for (int c = 0; c < num_configs; c++) {
    int num_threads = thread_counts[c];
    EndgameSolver *solver = endgame_solver_create();

    printf("\n--- Testing with %d thread(s) ---\n", num_threads);
    double start_time = get_time_sec();
    run_endgames_with_pv(config, solver, num_games, ply, num_threads, base_seed);
    double elapsed = get_time_sec() - start_time;
    printf("Total time for %d threads: %.3fs (%.3fs/game)\n",
           num_threads, elapsed, elapsed / num_games);

    endgame_solver_destroy(solver);
  }

  config_destroy(config);
}
