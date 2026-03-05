// Debug test: replay seed 7014, STATIC(p0) vs INFER1(p1)
// Focus on turn 2 where INFER1 picks VILLI 16 instead of VITELLIN 64.
//
// STATIC(p0) plays turn 1: 8D WIDTH 32
// INFER1(p1) plays turn 2: E4 VILLI 16 — WHY not VITELLIN 64?
//
// We replay up to turn 2, run inference + sim with inference,
// then run sim WITHOUT inference, and compare.
//
// Usage: ./bin/magpie_test debug_infer_turn2

#include "../src/def/bai_defs.h"
#include "../src/def/config_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/inference.h"
#include "../src/impl/simmer.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/str/sim_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_PLAYS 15
#define NUM_PLIES 2
#define NUM_THREADS 10
#define SIM_BUDGET_S 2.0
#define INFER_TIME_LIMIT_S 1.0

typedef struct {
  ThreadControl *tc;
  double seconds;
  volatile bool done;
} TimerArgs;

static void *timer_thread_func(void *arg) {
  TimerArgs *ta = (TimerArgs *)arg;
  double remaining = ta->seconds;
  while (remaining > 0 && !ta->done) {
    double sleep_time = remaining > 0.05 ? 0.05 : remaining;
    struct timespec ts;
    ts.tv_sec = (time_t)sleep_time;
    ts.tv_nsec = (long)((sleep_time - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
    remaining -= sleep_time;
  }
  if (!ta->done) {
    thread_control_set_status(ta->tc, THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
  return NULL;
}

void test_debug_infer_turn2(void) {
  setbuf(stdout, NULL);

  printf("\n=== Debug: seed 7014, STATIC(p0) vs INFER1(p1) ===\n");
  printf("=== Why does INFER1 play VILLI 16 instead of VITELLIN 64? ===\n\n");

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 4 -threads 10");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);

  ErrorStack *error_stack = error_stack_create();
  WinPct *win_pcts = win_pct_create(config_get_data_paths(config),
                                     DEFAULT_WIN_PCT, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    error_stack_destroy(error_stack);
    config_destroy(config);
    return;
  }

  ThreadControl *tc = config_get_thread_control(config);
  Game *game = game_duplicate(config_get_game(config));
  MoveList *move_list = move_list_create(NUM_PLAYS);
  SimResults *sim_results = sim_results_create(0.0);
  SimCtx *sim_ctx = NULL;
  InferenceResults *inference_results = inference_results_create(NULL);
  StringBuilder *sb = string_builder_create();

  // Set up seed 7014
  game_reset(game);
  game_seed(game, 7014);
  draw_starting_racks(game);

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  // Print starting racks
  printf("p0 (STATIC) rack: ");
  string_builder_clear(sb);
  string_builder_add_rack(sb, player_get_rack(game_get_player(game, 0)), ld, false);
  printf("%s\n", string_builder_peek(sb));

  printf("p1 (INFER1) rack: ");
  string_builder_clear(sb);
  string_builder_add_rack(sb, player_get_rack(game_get_player(game, 1)), ld, false);
  printf("%s\n\n", string_builder_peek(sb));

  // Save game state before turn 1
  Game *game_before_turn1 = game_duplicate(game);

  // Turn 1: STATIC (p0) plays equity-best
  printf("--- Turn 1: STATIC (p0) plays equity-best ---\n");
  {
    const MoveGenArgs gen_args = {
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
    generate_moves(&gen_args);

    const Move *static_move = move_list_get_move(move_list, 0);
    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), static_move, ld, true);
    printf("  STATIC plays: %s (equity %.1f)\n\n", string_builder_peek(sb),
           equity_to_double(move_get_equity(static_move)));

    Move saved_turn1_move = *static_move;
    play_move(static_move, game, NULL);

    // Now it's turn 2: INFER1 (p1)
    printf("=== Turn 2: INFER1 (p1) — THE CRITICAL TURN ===\n\n");

    // Show INFER1's rack
    printf("INFER1 rack: ");
    string_builder_clear(sb);
    string_builder_add_rack(sb, player_get_rack(game_get_player(game, 1)), ld, false);
    printf("%s\n\n", string_builder_peek(sb));

    // Generate moves for p1
    const MoveGenArgs gen_args2 = {
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
    generate_moves(&gen_args2);

    printf("Top 15 moves by equity:\n");
    int num_moves = move_list_get_count(move_list);
    for (int i = 0; i < num_moves && i < 15; i++) {
      const Move *m = move_list_get_move(move_list, i);
      string_builder_clear(sb);
      string_builder_add_move(sb, game_get_board(game), m, ld, true);
      printf("  %2d: %-25s (equity %7.1f, score %d)\n", i + 1,
             string_builder_peek(sb), equity_to_double(move_get_equity(m)),
             equity_to_int(move_get_score(m)));
    }
    printf("\n");

    // --- Run inference on STATIC's turn 1 move ---
    printf("Running inference on STATIC's opening WIDTH...\n");

    Rack target_played_tiles;
    rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
    Rack target_known_rack;
    rack_set_dist_size_and_reset(&target_known_rack, ld_size);
    Rack nontarget_known_rack;
    rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);
    // nontarget is p1 (INFER1) — copy p1's rack from game_before_turn1
    rack_copy(&nontarget_known_rack,
              player_get_rack(game_get_player(game_before_turn1, 1)));

    Equity score = move_get_score(&saved_turn1_move);
    const int tiles_length = move_get_tiles_length(&saved_turn1_move);
    for (int i = 0; i < tiles_length; i++) {
      const MachineLetter ml = move_get_tile(&saved_turn1_move, i);
      if (ml != PLAYED_THROUGH_MARKER) {
        if (get_is_blanked(ml)) {
          rack_add_letter(&target_played_tiles, BLANK_MACHINE_LETTER);
        } else {
          rack_add_letter(&target_played_tiles, ml);
        }
      }
    }

    InferenceArgs infer_args;
    infer_args_fill(&infer_args, NUM_PLAYS, int_to_equity(1), NULL,
                    game_before_turn1, NUM_THREADS, 0, tc, false, true,
                    0, // target = p0 (STATIC, who played WIDTH)
                    score, 0,
                    &target_played_tiles, &target_known_rack,
                    &nontarget_known_rack);

    thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
    TimerArgs ta_infer = {.tc = tc, .seconds = INFER_TIME_LIMIT_S, .done = false};
    pthread_t timer_infer;
    pthread_create(&timer_infer, NULL, timer_thread_func, &ta_infer);
    infer_without_ctx(&infer_args, inference_results, error_stack);
    ta_infer.done = true;
    pthread_join(timer_infer, NULL);

    if (!error_stack_is_empty(error_stack)) {
      printf("  INFERENCE ERROR:\n");
      error_stack_print_and_reset(error_stack);
    } else {
      bool interrupted =
          thread_control_get_status(tc) == THREAD_CONTROL_STATUS_USER_INTERRUPT;
      printf("  Inference completed%s.\n\n", interrupted ? " (INTERRUPTED)" : "");
    }

    // --- SIM WITH INFERENCE ---
    printf("========================================\n");
    printf("  SIM WITH INFERENCE (use_inference=true)\n");
    printf("========================================\n\n");

    // Rebuild inference_args for the simmer
    Rack ia_tp, ia_tk, ia_ntk;
    rack_set_dist_size_and_reset(&ia_tp, ld_size);
    rack_set_dist_size_and_reset(&ia_tk, ld_size);
    rack_set_dist_size_and_reset(&ia_ntk, ld_size);
    rack_copy(&ia_ntk,
              player_get_rack(game_get_player(game_before_turn1, 1)));

    for (int i = 0; i < tiles_length; i++) {
      const MachineLetter ml = move_get_tile(&saved_turn1_move, i);
      if (ml != PLAYED_THROUGH_MARKER) {
        if (get_is_blanked(ml))
          rack_add_letter(&ia_tp, BLANK_MACHINE_LETTER);
        else
          rack_add_letter(&ia_tp, ml);
      }
    }

    InferenceArgs sim_infer_args;
    infer_args_fill(&sim_infer_args, NUM_PLAYS, int_to_equity(1), NULL,
                    game_before_turn1, NUM_THREADS, 0, tc, false, true,
                    0, score, 0, &ia_tp, &ia_tk, &ia_ntk);

    SimArgs sim_args_with;
    sim_args_fill(NUM_PLIES, move_list, NULL, win_pcts, inference_results,
                  tc, game, true, false, NUM_THREADS, 0, NUM_PLAYS,
                  NUM_PLIES, 0, UINT64_MAX, 1, 101.0, BAI_THRESHOLD_NONE,
                  999, BAI_SAMPLING_RULE_TOP_TWO_IDS, -1.0,
                  &sim_infer_args, &sim_args_with);

    thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
    TimerArgs ta_sim1 = {.tc = tc, .seconds = SIM_BUDGET_S, .done = false};
    pthread_t timer_sim1;
    pthread_create(&timer_sim1, NULL, timer_thread_func, &ta_sim1);
    simulate(&sim_args_with, &sim_ctx, sim_results, error_stack);
    ta_sim1.done = true;
    pthread_join(timer_sim1, NULL);

    if (!error_stack_is_empty(error_stack)) {
      printf("  SIM ERROR:\n");
      error_stack_print_and_reset(error_stack);
    }

    char *sim_str1 = sim_results_get_string(
        game, sim_results, NUM_PLAYS, NUM_PLIES, -1, -1, NULL, 0,
        false, false, NULL);
    printf("%s\n", sim_str1);
    free(sim_str1);

    BAIResult *bai1 = sim_results_get_bai_result(sim_results);
    int best1 = bai_result_get_best_arm(bai1);
    if (best1 < 0) best1 = 0;
    const Move *chosen1 =
        simmed_play_get_move(sim_results_get_simmed_play(sim_results, best1));
    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), chosen1, ld, true);
    printf("  >>> Sim WITH inference chose (arm %d): %s\n\n", best1,
           string_builder_peek(sb));

    // --- SIM WITHOUT INFERENCE ---
    printf("========================================\n");
    printf("  SIM WITHOUT INFERENCE (use_inference=false)\n");
    printf("========================================\n\n");

    SimArgs sim_args_without;
    sim_args_fill(NUM_PLIES, move_list, NULL, win_pcts, inference_results,
                  tc, game, false, false, NUM_THREADS, 0, NUM_PLAYS,
                  NUM_PLIES, 0, UINT64_MAX, 1, 101.0, BAI_THRESHOLD_NONE,
                  999, BAI_SAMPLING_RULE_TOP_TWO_IDS, -1.0,
                  NULL, &sim_args_without);

    thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
    TimerArgs ta_sim2 = {.tc = tc, .seconds = SIM_BUDGET_S, .done = false};
    pthread_t timer_sim2;
    pthread_create(&timer_sim2, NULL, timer_thread_func, &ta_sim2);
    simulate(&sim_args_without, &sim_ctx, sim_results, error_stack);
    ta_sim2.done = true;
    pthread_join(timer_sim2, NULL);

    if (!error_stack_is_empty(error_stack)) {
      printf("  SIM ERROR:\n");
      error_stack_print_and_reset(error_stack);
    }

    char *sim_str2 = sim_results_get_string(
        game, sim_results, NUM_PLAYS, NUM_PLIES, -1, -1, NULL, 0,
        false, false, NULL);
    printf("%s\n", sim_str2);
    free(sim_str2);

    BAIResult *bai2 = sim_results_get_bai_result(sim_results);
    int best2 = bai_result_get_best_arm(bai2);
    if (best2 < 0) best2 = 0;
    const Move *chosen2 =
        simmed_play_get_move(sim_results_get_simmed_play(sim_results, best2));
    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), chosen2, ld, true);
    printf("  >>> Sim WITHOUT inference chose (arm %d): %s\n\n", best2,
           string_builder_peek(sb));

    // --- SIM WITHOUT INFERENCE, sample_minimum=100 ---
    printf("========================================\n");
    printf("  SIM WITHOUT INFERENCE, sample_minimum=100\n");
    printf("========================================\n\n");

    SimArgs sim_args_minp100;
    sim_args_fill(NUM_PLIES, move_list, NULL, win_pcts, inference_results,
                  tc, game, false, false, NUM_THREADS, 0, NUM_PLAYS,
                  NUM_PLIES, 0, UINT64_MAX, 100, 101.0, BAI_THRESHOLD_NONE,
                  999, BAI_SAMPLING_RULE_TOP_TWO_IDS, -1.0,
                  NULL, &sim_args_minp100);

    thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);
    TimerArgs ta_sim3 = {.tc = tc, .seconds = SIM_BUDGET_S, .done = false};
    pthread_t timer_sim3;
    pthread_create(&timer_sim3, NULL, timer_thread_func, &ta_sim3);
    simulate(&sim_args_minp100, &sim_ctx, sim_results, error_stack);
    ta_sim3.done = true;
    pthread_join(timer_sim3, NULL);

    if (!error_stack_is_empty(error_stack)) {
      printf("  SIM ERROR:\n");
      error_stack_print_and_reset(error_stack);
    }

    char *sim_str3 = sim_results_get_string(
        game, sim_results, NUM_PLAYS, NUM_PLIES, -1, -1, NULL, 0,
        false, false, NULL);
    printf("%s\n", sim_str3);
    free(sim_str3);

    BAIResult *bai3 = sim_results_get_bai_result(sim_results);
    int best3 = bai_result_get_best_arm(bai3);
    if (best3 < 0) best3 = 0;
    const Move *chosen3 =
        simmed_play_get_move(sim_results_get_simmed_play(sim_results, best3));
    string_builder_clear(sb);
    string_builder_add_move(sb, game_get_board(game), chosen3, ld, true);
    printf("  >>> Sim minp=100 chose (arm %d): %s\n\n", best3,
           string_builder_peek(sb));
  }

  // Cleanup
  game_destroy(game_before_turn1);
  sim_ctx_destroy(sim_ctx);
  inference_results_destroy(inference_results);
  sim_results_destroy(sim_results);
  move_list_destroy(move_list);
  game_destroy(game);
  win_pct_destroy(win_pcts);
  string_builder_destroy(sb);
  error_stack_destroy(error_stack);
  config_destroy(config);
}
