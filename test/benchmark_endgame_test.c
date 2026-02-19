#include "benchmark_endgame_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/endgame_string.h"
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
#include <string.h>
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

// A/B benchmark player config: either an endgame solver or greedy movegen.
typedef struct {
  int plies;        // 0 = static eval (greedy movegen)
  bool heuristics;  // only meaningful when plies > 0
  move_sort_t sort; // only meaningful when plies == 0
} ABPlayerConfig;

typedef struct {
  ABPlayerConfig a;
  ABPlayerConfig b;
  int num_positions;
  uint64_t seed;
} ABBenchmarkConfig;

static void ab_player_label(const ABPlayerConfig *cfg, char *buf, size_t len) {
  if (cfg->plies > 0) {
    (void)snprintf(buf, len, "%d-ply %s", cfg->plies,
                   cfg->heuristics ? "heuristic" : "classic");
  } else {
    (void)snprintf(buf, len, "%s movegen",
                   cfg->sort == MOVE_SORT_SCORE ? "score" : "equity");
  }
}

// Play out an endgame position move-by-move with A/B player configs.
// Each turn, the on-turn player uses either endgame solver (plies > 0) or
// greedy movegen (plies == 0) according to its config.
// Returns final spread from player_a_idx's perspective.
static int play_endgame_generic(Game *game, const ABPlayerConfig *cfg_a,
                                EndgameSolver *solver_a, EndgameArgs *args_a,
                                const ABPlayerConfig *cfg_b,
                                EndgameSolver *solver_b, EndgameArgs *args_b,
                                EndgameResults *results, MoveList *move_list,
                                double *time_a, double *time_b,
                                int player_a_idx) {
  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
    int on_turn = game_get_player_on_turn_index(game);
    bool is_a = (on_turn == player_a_idx);
    const ABPlayerConfig *cfg = is_a ? cfg_a : cfg_b;
    double *elapsed = is_a ? time_a : time_b;

    Timer t;
    ctimer_start(&t);

    if (cfg->plies > 0) {
      EndgameSolver *solver = is_a ? solver_a : solver_b;
      EndgameArgs *args = is_a ? args_a : args_b;
      args->game = game;
      args->plies = cfg->plies;

      ErrorStack *err = error_stack_create();
      endgame_solve(solver, args, results, err);
      *elapsed += ctimer_elapsed_seconds(&t);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      const PVLine *pv =
          endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
      if (pv->num_moves == 0) {
        break;
      }
      SmallMove best = pv->moves[0];
      small_move_to_move(move_list->spare_move, &best, game_get_board(game));
      play_move(move_list->spare_move, game, NULL);
    } else {
      move_list_reset(move_list);
      const MoveGenArgs mg_args = {
          .game = game,
          .move_list = move_list,
          .move_record_type = MOVE_RECORD_BEST,
          .move_sort_type = cfg->sort,
          .override_kwg = NULL,
          .thread_index = 0,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&mg_args);
      *elapsed += ctimer_elapsed_seconds(&t);
      if (move_list->count == 0) {
        break;
      }
      play_move(move_list->moves[0], game, NULL);
    }
  }
  int score_a =
      equity_to_int(player_get_score(game_get_player(game, player_a_idx)));
  int score_b =
      equity_to_int(player_get_score(game_get_player(game, 1 - player_a_idx)));
  return score_a - score_b;
}

// Run a single A/B benchmark matchup from a config entry.
static void run_benchmark_ab_config(const ABBenchmarkConfig *bench) {
  log_set_level(LOG_WARN);

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");

  EndgameSolver *solver_a = bench->a.plies > 0 ? endgame_solver_create() : NULL;
  EndgameSolver *solver_b = bench->b.plies > 0 ? endgame_solver_create() : NULL;

  EndgameArgs args_a = {0};
  EndgameArgs args_b = {0};

  if (solver_a) {
    args_a = (EndgameArgs){
        .thread_control = config_get_thread_control(config),
        .plies = bench->a.plies,
        .tt_fraction_of_mem = 0.10,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 6,
        .num_top_moves = 1,
        .use_heuristics = bench->a.heuristics,
        .per_ply_callback = NULL,
        .per_ply_callback_data = NULL,
    };
  }

  if (solver_b) {
    args_b = (EndgameArgs){
        .thread_control = config_get_thread_control(config),
        .plies = bench->b.plies,
        .tt_fraction_of_mem = 0.10,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 6,
        .num_top_moves = 1,
        .use_heuristics = bench->b.heuristics,
        .per_ply_callback = NULL,
        .per_ply_callback_data = NULL,
    };
  }

  EndgameResults *results = endgame_results_create();
  MoveList *move_list = move_list_create(1);

  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  char label_a[64];
  char label_b[64];
  ab_player_label(&bench->a, label_a, sizeof(label_a));
  ab_player_label(&bench->b, label_b, sizeof(label_b));

  int total_a_spread = 0;
  int total_b_spread = 0;
  double total_time_a = 0;
  double total_time_b = 0;
  int valid_positions = 0;
  int a_wins = 0;
  int b_wins = 0;
  int ties = 0;
  const int max_attempts = bench->num_positions * 10;

  printf("\n");
  printf("==============================================================\n");
  printf("  A/B Endgame Benchmark: %d positions, game pairs\n",
         bench->num_positions);
  printf("  A: %s\n", label_a);
  printf("  B: %s\n", label_b);
  printf("==============================================================\n\n");

  for (int i = 0; valid_positions < bench->num_positions && i < max_attempts;
       i++) {
    game_reset(game);
    game_seed(game, bench->seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    int on_turn = game_get_player_on_turn_index(game);
    valid_positions++;

    Game *saved_game = game_duplicate(game);

    // Game 1: A plays as on-turn, B plays as opponent
    double time_a_1 = 0;
    double time_b_1 = 0;
    int spread_1 = play_endgame_generic(
        game, &bench->a, solver_a, &args_a, &bench->b, solver_b, &args_b,
        results, move_list, &time_a_1, &time_b_1, on_turn);

    // Game 2: B plays as on-turn, A plays as opponent
    game_copy(game, saved_game);
    double time_a_2 = 0;
    double time_b_2 = 0;
    // NOLINTNEXTLINE(readability-suspicious-call-argument)
    int spread_2 = play_endgame_generic(
        game, &bench->b, solver_b, &args_b, &bench->a, solver_a, &args_a,
        results, move_list, &time_b_2, &time_a_2, on_turn);

    // spread_1 is from A's perspective, spread_2 is from B's perspective
    int pair_advantage = spread_1 - spread_2;
    total_a_spread += spread_1;
    total_b_spread += spread_2;
    total_time_a += time_a_1 + time_a_2;
    total_time_b += time_b_1 + time_b_2;

    if (pair_advantage > 0) {
      a_wins++;
    } else if (pair_advantage < 0) {
      b_wins++;
    } else {
      ties++;
    }

    printf("  Pair %3d: A=%+4d, B=%+4d, "
           "adv=%+4d  (A: %.2fs, B: %.2fs)\n",
           valid_positions, spread_1, spread_2, pair_advantage,
           time_a_1 + time_a_2, time_b_1 + time_b_2);
    (void)fflush(stdout);

    game_destroy(saved_game);
  }

  assert(valid_positions == bench->num_positions);

  int net_advantage = total_a_spread - total_b_spread;
  printf("\n==============================================================\n");
  printf("  RESULTS: %d game pairs (%d games total)\n", bench->num_positions,
         bench->num_positions * 2);
  printf("  A (%s) total spread: %+d (avg %+.1f)\n", label_a, total_a_spread,
         (double)total_a_spread / bench->num_positions);
  printf("  B (%s) total spread: %+d (avg %+.1f)\n", label_b, total_b_spread,
         (double)total_b_spread / bench->num_positions);
  printf("  Net advantage (A): %+d (avg %+.1f per pair)\n", net_advantage,
         (double)net_advantage / bench->num_positions);
  printf("  Pair wins: A=%d, B=%d, ties=%d\n", a_wins, b_wins, ties);
  printf("  Time: A=%.2fs, B=%.2fs\n", total_time_a, total_time_b);
  printf("==============================================================\n");

  endgame_results_destroy(results);
  move_list_destroy(move_list);
  if (solver_a) {
    endgame_solver_destroy(solver_a);
  }
  if (solver_b) {
    endgame_solver_destroy(solver_b);
  }
  config_destroy(config);
}

static const ABBenchmarkConfig ab_configs[] = {
    // score movegen vs 2-ply heuristic
    {{0, false, MOVE_SORT_SCORE}, {2, true, 0}, 50, 42},
    // 3-ply heuristic vs 3-ply classic
    {{3, true, 0}, {3, false, 0}, 50, 42},
    // 2-ply heuristic vs 3-ply classic
    {{2, true, 0}, {3, false, 0}, 50, 42},
    // 3-ply heuristic vs 4-ply classic
    {{3, true, 0}, {4, false, 0}, 50, 42},
    // 4-ply heuristic vs 5-ply heuristic
    {{4, true, 0}, {5, true, 0}, 50, 42},
};

void test_benchmark_endgame_ab(void) {
  int n = (int)(sizeof(ab_configs) / sizeof(ab_configs[0]));
  for (int i = 0; i < n; i++) {
    run_benchmark_ab_config(&ab_configs[i]);
  }
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

      const PVLine *pv =
          endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
      if (pv->num_moves == 0) {
        break;
      }

      SmallMove best = pv->moves[0];
      small_move_to_move(move_list->spare_move, &best, game_get_board(game));
      play_move(move_list->spare_move, game, NULL);
    }

    total_time += game_time;
    int score0 = equity_to_int(player_get_score(game_get_player(game, 0)));
    int score1 = equity_to_int(player_get_score(game_get_player(game, 1)));
    printf("  Game %3d: %d-%d (spread %+d)  %.2fs\n", valid_positions, score0,
           score1, score0 - score1, game_time);
  }

  assert(valid_positions == num_positions);

  printf("\n==============================================================\n");
  printf("  TOTAL: %.2fs for %d games (avg %.2fs)\n", total_time, num_positions,
         total_time / num_positions);
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
                                         int num_counts, uint64_t base_seed) {
  log_set_level(LOG_WARN);

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  // Collect endgame positions by playing games until bag is empty.
  Game **saved_games = malloc_or_die(sizeof(Game *) * (size_t)num_positions);
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

        const PVLine *pv =
            endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
        if (pv->num_moves == 0) {
          break;
        }

        SmallMove best = pv->moves[0];
        small_move_to_move(move_list->spare_move, &best, game_get_board(game));
        play_move(move_list->spare_move, game, NULL);
      }
    }

    printf("  %d thread%s: %.2fs (avg %.3fs/game)\n", nthreads,
           nthreads == 1 ? " " : "s", total_time, total_time / num_positions);
    (void)fflush(stdout);

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
  int total = 0;
  int stuck = 0;
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
  if (total == 0) {
    return 0.0F;
  }
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
  FILE *cgp_stuck = fopen("/tmp/stuck_tile_positions.cgp", "we");
  FILE *cgp_clean = fopen("/tmp/clean_positions.cgp", "we");
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

      if (max_frac > 0.0F) {
        any_stuck_at_depth[d]++;
        found_stuck = true;
      }

      int bucket;
      if (max_frac == 0.0F) {
        bucket = 0;
      } else if (max_frac < 0.20F) {
        bucket = 1;
      } else if (max_frac < 0.40F) {
        bucket = 2;
      } else if (max_frac < 0.60F) {
        bucket = 3;
      } else if (max_frac < 0.80F) {
        bucket = 4;
      } else if (max_frac < 1.00F) {
        bucket = 5;
      } else {
        bucket = 6;
      }
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
        if (move_list->count == 0) {
          break;
        }
        play_move(move_list->moves[0], game, NULL);
      }
    }

    char *cgp = game_get_cgp(saved, true);
    if (found_stuck) {
      ever_stuck++;
      (void)fprintf(cgp_stuck, "%s\n", cgp);
      stuck_count++;
    } else {
      (void)fprintf(cgp_clean, "%s\n", cgp);
      clean_count++;
    }
    free(cgp);

    game_copy(game, saved);
    game_destroy(saved);
  }

  (void)fclose(cgp_stuck);
  (void)fclose(cgp_clean);
  assert(valid_positions == num_positions);

  printf("\n");
  printf("==============================================================\n");
  printf("  Stuck Tile Survey: %d positions, depths 0-1 (seed=%llu)\n",
         num_positions, (unsigned long long)base_seed);
  printf("==============================================================\n\n");
  printf("  Positions with stuck tiles at depth 0 or 1: %d/%d (%.1f%%)\n",
         ever_stuck, num_positions, 100.0 * ever_stuck / num_positions);
  printf("  Stuck:  /tmp/stuck_tile_positions.cgp (%d positions)\n",
         stuck_count);
  printf("  Clean:  /tmp/clean_positions.cgp (%d positions)\n\n", clean_count);

  const char *labels[] = {"    0%", " 1-19%", "20-39%", "40-59%",
                          "60-79%", "80-99%", "  100%"};

  for (int d = 0; d <= 1; d++) {
    int reached = 0;
    for (int b = 0; b < NUM_BUCKETS; b++) {
      reached += buckets[d][b];
    }
    if (reached == 0) {
      break;
    }

    printf("  Depth %d: %d/%d have stuck tiles (%.1f%%)\n", d,
           any_stuck_at_depth[d], reached,
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
  (void)fflush(stdout);

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
    FILE *check = fopen(dst, "re");
    if (!check) {
      FILE *in = fopen(src, "rbe");
      assert(in);
      FILE *out = fopen(dst, "wbe");
      assert(out);
      char buf[8192];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        (void)fwrite(buf, 1, n, out);
      }
      (void)fclose(in);
      (void)fclose(out);
    } else {
      (void)fclose(check);
    }
  }

  char lex_cmd[256];
  (void)snprintf(
      lex_cmd, sizeof(lex_cmd),
      "set -lex %s -threads 1 -s1 score -s2 score -r1 small -r2 small", lex);
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
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        break;
      }

      for (int p = 0; p < 2; p++) {
        const Rack *rack = player_get_rack(game_get_player(game, p));
        if (rack_get_total_letters(rack) == 0) {
          continue;
        }

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
        if (move_list->count == 0) {
          break;
        }
        play_move(move_list->moves[0], game, NULL);
      }
    }

    if (found_stuck) {
      stuck_found++;
    }
    game_copy(game, saved);
    game_destroy(saved);
  }

  // Sort by stuck count descending
  int order[MAX_ALPHABET_SIZE];
  for (int i = 0; i < ld_size; i++) {
    order[i] = i;
  }
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
    if (stuck_counts[ml] == 0) {
      break;
    }
    char *letter = ld_ml_to_hl(ld, ml);
    float rate = rack_counts[ml] > 0
                     ? 100.0F * (float)stuck_counts[ml] / (float)rack_counts[ml]
                     : 0.0F;
    printf("  %-8s %6d  %6d  %7.1f%%\n", letter, stuck_counts[ml],
           rack_counts[ml], (double)rate);
    free(letter);
  }

  printf("\n  Total stuck tile instances: %d\n", total_stuck_tiles);
  printf("  Stuck rate: %.1f%% of endgame positions\n",
         100.0 * stuck_found / games_tried);
  printf("==============================================================\n");
  (void)fflush(stdout);

  move_list_destroy(move_list);
  config_destroy(config);
}

void test_benchmark_cross_set_pruning(void) {
  log_set_level(LOG_WARN);

  const int num_games = 10;
  const int ply = 5;
  const int num_threads = 10;
  const uint64_t base_seed = 42;

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 10 -s1 score -s2 score -r1 small -r2 small");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  // Collect endgame positions
  Game **saved_games = malloc_or_die(sizeof(Game *) * (size_t)num_games);
  int valid = 0;
  const int max_attempts = num_games * 10;
  for (int i = 0; valid < num_games && i < max_attempts; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);
    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }
    saved_games[valid] = game_duplicate(game);
    valid++;
  }
  assert(valid == num_games);

  printf("\n");
  printf("==============================================================\n");
  printf("  Cross-Set Pruning Benchmark: %d positions, %d-ply, %d threads\n",
         num_games, ply, num_threads);
  printf("==============================================================\n\n");

  // Run both variants
  for (int variant = 0; variant < 2; variant++) {
    bool skip_pruned = (variant == 1);
    const char *label = skip_pruned ? "WITHOUT pruned cross-sets (old)"
                                    : "WITH pruned cross-sets (new)";

    EndgameSolver *solver = endgame_solver_create();
    EndgameResults *results = endgame_results_create();

    double total_time = 0;

    for (int p = 0; p < num_games; p++) {
      game_copy(game, saved_games[p]);

      EndgameArgs args = {.thread_control = config_get_thread_control(config),
                          .game = game,
                          .plies = ply,
                          .tt_fraction_of_mem = 0.10,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = num_threads,
                          .num_top_moves = 1,
                          .use_heuristics = true,
                          .per_ply_callback = NULL,
                          .per_ply_callback_data = NULL,
                          .skip_pruned_cross_sets = skip_pruned};

      Timer t;
      ctimer_start(&t);
      ErrorStack *err = error_stack_create();
      endgame_solve(solver, &args, results, err);
      double elapsed = ctimer_elapsed_seconds(&t);
      total_time += elapsed;
      assert(error_stack_is_empty(err));

      const PVLine *pv =
          endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
      printf("  [%s] Game %2d: value=%+4d  %.2fs\n",
             skip_pruned ? "old" : "new", p + 1, pv->score, elapsed);
      (void)fflush(stdout);
      error_stack_destroy(err);
    }

    printf("\n  %s\n", label);
    printf("  TOTAL: %.2fs  AVG: %.3fs/game\n\n", total_time,
           total_time / num_games);

    endgame_results_destroy(results);
    endgame_solver_destroy(solver);
  }

  printf("==============================================================\n");

  for (int p = 0; p < num_games; p++) {
    game_destroy(saved_games[p]);
  }
  free(saved_games);
  move_list_destroy(move_list);
  config_destroy(config);
}
