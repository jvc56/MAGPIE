// Game-pair autoplay benchmark: static BAI vs nested-sim BAI.
// Usage: ./bin/magpie_test gamepairbai
//
// Plays 10 game pairs from an empty board. Each pair uses the same seed
// but swaps which player uses the static strategy vs the nested strategy.
// Every turn: generate 15 candidates → BAI with fixed time limit → play best.
// BAI uses NO threshold (BAI_THRESHOLD_NONE), only the time limit.

#include "../src/def/config_defs.h"
#include "../src/compat/ctime.h"
#include "../src/def/bai_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/sim_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/simmer.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>

#define NUM_GAME_PAIRS 10
#define NUM_PLAYS 15
#define NUM_PLIES 2
#define NUM_THREADS 16
#define TURN_TIME_LIMIT_S 15

// Two strategies to compare
static const FidelityLevel STRATEGY_STATIC = {
    .sample_limit = UINT64_MAX,
    .sample_minimum = 1,
    .time_limit_seconds = TURN_TIME_LIMIT_S,
    .ply_strategy = PLY_STRATEGY_STATIC,
};

static const FidelityLevel STRATEGY_NESTED = {
    .sample_limit = UINT64_MAX,
    .sample_minimum = 1,
    .time_limit_seconds = TURN_TIME_LIMIT_S,
    .ply_strategy = PLY_STRATEGY_NESTED_SIM,
    .nested_candidates = 5,
    .nested_rollouts = 8,
    .nested_plies = 4,
};

typedef struct {
  int p0_wins;
  int p1_wins;
  int ties;
  int total_turns;
  double total_elapsed_s;
} PairResults;

// Play one move using BAI with the given fidelity level.
// Returns the move played (pointer into move_list, valid until next gen).
static const Move *play_sim_turn(Game *game, MoveList *move_list,
                                 SimResults *sim_results, SimCtx **sim_ctx,
                                 WinPct *win_pcts, ThreadControl *tc,
                                 const FidelityLevel *strategy,
                                 ErrorStack *error_stack) {
  // Generate candidate moves
  const MoveGenArgs gen_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&gen_args);

  const int num_moves = move_list_get_count(move_list);
  if (num_moves <= 1) {
    // Only one move (or pass) - just play it
    const Move *move = move_list_get_move(move_list, 0);
    play_move(move, game, NULL);
    return move;
  }

  // Build SimArgs with fixed time limit, no threshold
  SimArgs sim_args;
  sim_args_fill(
      NUM_PLIES, move_list,
      /* known_opp_rack */ NULL, win_pcts,
      /* inference_results */ NULL, tc, game,
      /* sim_with_inference */ false,
      /* use_heat_map */ false, NUM_THREADS,
      /* print_interval */ 0,
      /* max_num_display_plays */ NUM_PLAYS,
      /* max_num_display_plies */ NUM_PLIES,
      /* seed */ 0,
      /* max_iterations */ UINT64_MAX,
      /* min_play_iterations */ 1,
      /* scond */ 101.0,  // > 100 → BAI_THRESHOLD_NONE
      BAI_THRESHOLD_NONE,
      strategy->time_limit_seconds,
      BAI_SAMPLING_RULE_TOP_TWO_IDS,
      /* cutoff */ 0.0,
      /* inference_args */ NULL, &sim_args);

  // Override fidelity to use our strategy
  sim_args.num_fidelity_levels = 1;
  sim_args.fidelity_levels[0] = *strategy;

  thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
  simulate(&sim_args, sim_ctx, sim_results, error_stack);

  // Get best arm
  BAIResult *bai_result = sim_results_get_bai_result(sim_results);
  int best_arm = bai_result_get_best_arm(bai_result);
  if (best_arm < 0) {
    best_arm = 0;
  }

  const Move *best_move =
      simmed_play_get_move(sim_results_get_simmed_play(sim_results, best_arm));
  play_move(best_move, game, NULL);
  return best_move;
}

// Play a full game. p0_strategy and p1_strategy control fidelity per player.
static void play_game(Game *game, MoveList *move_list, SimResults *sim_results,
                      SimCtx **sim_ctx, WinPct *win_pcts, ThreadControl *tc,
                      const FidelityLevel *p0_strategy,
                      const FidelityLevel *p1_strategy,
                      const char *p0_label, const char *p1_label,
                      int game_num, uint64_t seed, PairResults *results) {
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);

  ErrorStack *error_stack = error_stack_create();
  struct timespec game_start, game_end;
  clock_gettime(CLOCK_MONOTONIC, &game_start);

  int turn = 0;
  while (!game_over(game)) {
    int player_idx = game_get_player_on_turn_index(game);
    const FidelityLevel *strategy =
        (player_idx == 0) ? p0_strategy : p1_strategy;

    play_sim_turn(game, move_list, sim_results, sim_ctx, win_pcts, tc,
                  strategy, error_stack);
    turn++;

    if (!error_stack_is_empty(error_stack)) {
      printf("  ERROR on turn %d: ", turn);
      error_stack_print_and_reset(error_stack);
      break;
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &game_end);
  double elapsed =
      (game_end.tv_sec - game_start.tv_sec) +
      (game_end.tv_nsec - game_start.tv_nsec) / 1e9;

  int p0_score = equity_to_int(
      player_get_score(game_get_player(game, 0)));
  int p1_score = equity_to_int(
      player_get_score(game_get_player(game, 1)));

  const char *winner_label;
  if (p0_score > p1_score) {
    results->p0_wins++;
    winner_label = p0_label;
  } else if (p1_score > p0_score) {
    results->p1_wins++;
    winner_label = p1_label;
  } else {
    results->ties++;
    winner_label = "TIE";
  }
  results->total_turns += turn;
  results->total_elapsed_s += elapsed;

  printf("  Game %2d: %s(%d) vs %s(%d) → %s  [%d turns, %.1fs]\n",
         game_num, p0_label, p0_score, p1_label, p1_score,
         winner_label, turn, elapsed);

  error_stack_destroy(error_stack);
}

void test_gamepair_bai_benchmark(void) {
  setbuf(stdout, NULL);  // Disable stdout buffering for real-time output
  printf("\n");
  printf("================================================\n");
  printf("  Game-Pair BAI Benchmark\n");
  printf("  %d game pairs, %ds/turn, %d threads\n",
         NUM_GAME_PAIRS, TURN_TIME_LIMIT_S, NUM_THREADS);
  printf("  Static vs Nested (K=5 N=8 plies=4)\n");
  printf("================================================\n");

  // Create config and load game data via a CGP
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 16");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  // Load win_pcts directly (avoid throwaway sim on empty board)
  ErrorStack *load_es = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                     DEFAULT_WIN_PCT, load_es);
  if (!error_stack_is_empty(load_es)) {
    error_stack_print_and_reset(load_es);
    error_stack_destroy(load_es);
    config_destroy(config);
    return;
  }
  error_stack_destroy(load_es);

  ThreadControl *tc = config_get_thread_control(config);

  // Create game resources
  Game *game = game_duplicate(config_get_game(config));
  MoveList *move_list = move_list_create(NUM_PLAYS);
  SimResults *sim_results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;

  PairResults static_as_p0 = {0};  // static=P0, nested=P1
  PairResults nested_as_p0 = {0};  // nested=P0, static=P1

  printf("\n--- Games ---\n");
  for (int pair = 0; pair < NUM_GAME_PAIRS; pair++) {
    uint64_t seed = 1000 + (uint64_t)pair;

    // Game A: Static=P0, Nested=P1
    play_game(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
              &STRATEGY_STATIC, &STRATEGY_NESTED,
              "STATIC", "NESTED", pair * 2 + 1, seed, &static_as_p0);

    // Game B: Nested=P0, Static=P1 (same seed, swapped strategies)
    play_game(game, move_list, sim_results, &sim_ctx, win_pcts, tc,
              &STRATEGY_NESTED, &STRATEGY_STATIC,
              "NESTED", "STATIC", pair * 2 + 2, seed, &nested_as_p0);
  }

  // Aggregate: count wins by strategy, not by player position
  int static_wins = static_as_p0.p0_wins + nested_as_p0.p1_wins;
  int nested_wins = static_as_p0.p1_wins + nested_as_p0.p0_wins;
  int ties = static_as_p0.ties + nested_as_p0.ties;
  int total_games = NUM_GAME_PAIRS * 2;
  int total_turns = static_as_p0.total_turns + nested_as_p0.total_turns;
  double total_elapsed =
      static_as_p0.total_elapsed_s + nested_as_p0.total_elapsed_s;

  printf("\n================================================\n");
  printf("  RESULTS (%d games = %d pairs)\n", total_games, NUM_GAME_PAIRS);
  printf("  Static wins: %d (%.1f%%)\n", static_wins,
         100.0 * static_wins / total_games);
  printf("  Nested wins: %d (%.1f%%)\n", nested_wins,
         100.0 * nested_wins / total_games);
  printf("  Ties:        %d\n", ties);
  printf("  Avg turns/game: %.1f\n", (double)total_turns / total_games);
  printf("  Total wall time: %.1fs (%.1fs/game avg)\n",
         total_elapsed, total_elapsed / total_games);
  printf("================================================\n");

  sim_ctx_destroy(sim_ctx);
  sim_results_destroy(sim_results);
  move_list_destroy(move_list);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  config_destroy(config);
}
