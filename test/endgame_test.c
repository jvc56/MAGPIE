#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/str/endgame_string.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Helper: append [Px wins by Y] / [Tie] based on spread delta
static void append_outcome(StringBuilder *sb, const Game *game,
                           int32_t spread_delta) {
  int on_turn = game_get_player_on_turn_index(game);
  int p1 = equity_to_int(player_get_score(game_get_player(game, 0)));
  int p2 = equity_to_int(player_get_score(game_get_player(game, 1)));
  int final_spread = (p1 - p2) + (on_turn == 0 ? spread_delta : -spread_delta);
  if (final_spread > 0) {
    string_builder_add_formatted_string(sb, " [P1 wins by %d]", final_spread);
  } else if (final_spread < 0) {
    string_builder_add_formatted_string(sb, " [P2 wins by %d]", -final_spread);
  } else {
    string_builder_add_string(sb, " [Tie]");
  }
}

// Format a PVLine into a string builder (moves separated by spaces, with
// end-of-game annotations and | between negamax/greedy moves)
static void format_pvline(StringBuilder *sb, const PVLine *pv_line,
                          const Game *game) {
  const LetterDistribution *ld = game_get_ld(game);
  Move move;
  Game *gc = game_duplicate(game);
  for (int i = 0; i < pv_line->num_moves; i++) {
    small_move_to_move(&move, &(pv_line->moves[i]), game_get_board(gc));
    string_builder_add_move(sb, game_get_board(gc), &move, ld, true);
    play_move(&move, gc, NULL);
    if (game_get_game_end_reason(gc) == GAME_END_REASON_STANDARD) {
      int opp_idx = game_get_player_on_turn_index(gc);
      const Rack *opp_rack = player_get_rack(game_get_player(gc, opp_idx));
      int adj = equity_to_int(calculate_end_rack_points(opp_rack, ld));
      string_builder_add_string(sb, " (");
      string_builder_add_rack(sb, opp_rack, ld, false);
      string_builder_add_formatted_string(sb, " +%d)", adj);
    } else if (game_get_game_end_reason(gc) ==
               GAME_END_REASON_CONSECUTIVE_ZEROS) {
      string_builder_add_string(sb, " (6 zeros)");
    }
    if (i < pv_line->num_moves - 1) {
      if (i + 1 == pv_line->negamax_depth && pv_line->negamax_depth > 0) {
        string_builder_add_string(sb, " |");
      }
      string_builder_add_string(sb, " ");
    }
  }
  game_destroy(gc);
}

// Per-ply callback to print PV and ranked root moves during iterative
// deepening
static void print_pv_callback(int depth, int32_t value, const PVLine *pv_line,
                              const Game *game, const PVLine *ranked_pvs,
                              int num_ranked_pvs, void *user_data) {
  const Timer *timer = (const Timer *)user_data;
  double elapsed = ctimer_elapsed_seconds(timer);

  // Print best PV line
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb, "  depth %d: value=%d, time=%.3fs, pv=", depth, value, elapsed);
  format_pvline(sb, pv_line, game);
  append_outcome(sb, game, value);
  printf("%s\n", string_builder_peek(sb));
  string_builder_destroy(sb);

  (void)ranked_pvs;
  (void)num_ranked_pvs;
}

// Per-ply callback that also prints ranked root moves
static void print_pv_and_ranked_callback(int depth, int32_t value,
                                         const PVLine *pv_line,
                                         const Game *game,
                                         const PVLine *ranked_pvs,
                                         int num_ranked_pvs, void *user_data) {
  print_pv_callback(depth, value, pv_line, game, ranked_pvs, num_ranked_pvs,
                    user_data);
  for (int i = 0; i < num_ranked_pvs; i++) {
    StringBuilder *sb = string_builder_create();
    string_builder_add_formatted_string(sb, "    %2d. value=%d, pv=", i + 1,
                                        ranked_pvs[i].score);
    format_pvline(sb, &ranked_pvs[i], game);
    append_outcome(sb, game, ranked_pvs[i].score);
    printf("%s\n", string_builder_peek(sb));
    string_builder_destroy(sb);
  }
}

typedef struct {
  Config *config;
  int timeout;
} TimeoutThreadArgs;

static void *timeout_thread_function(void *arg) {
  const TimeoutThreadArgs *args = (TimeoutThreadArgs *)arg;
  char *endgame_string = endgame_results_get_string(
      config_get_endgame_results(args->config), config_get_game(args->config),
      config_get_game_history(args->config));
  free(endgame_string);
  for (int i = 0; i < args->timeout; i++) {
    ctime_nap(1);
    endgame_string = endgame_results_get_string(
        config_get_endgame_results(args->config), config_get_game(args->config),
        config_get_game_history(args->config));
    printf("%s\n", endgame_string);
    free(endgame_string);
  }
  thread_control_set_status(config_get_thread_control(args->config),
                            THREAD_CONTROL_STATUS_USER_INTERRUPT);
  return NULL;
}

void test_single_endgame(const char *config_settings, const char *cgp,
                         int initial_small_move_arena_size,
                         error_code_t expected_error_code,
                         const int expected_score, const bool is_pass,
                         int timeout) {
  // Load config
  Config *config = config_create_or_die(config_settings);
  load_and_exec_config_or_die(config, cgp);

  // Create solver
  EndgameCtx *endgame_ctx = NULL;

  // Create args
  Game *game = config_get_game(config);
  Timer timer;
  ctimer_start(&timer);
  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = config_get_endgame_plies(config);
  endgame_args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
  endgame_args.initial_small_move_arena_size = initial_small_move_arena_size;
  endgame_args.num_threads = 6;
  endgame_args.use_heuristics = true;
  endgame_args.forced_pass_bypass = true;
  endgame_args.enable_pv_display = true;
  endgame_args.num_top_moves = 1;
  endgame_args.per_ply_callback = print_pv_callback;
  endgame_args.per_ply_callback_data = &timer;
  endgame_args.seed = 42;

  if (timeout > 0) {
    endgame_args.num_top_moves = 10;
  }

  // Create results
  EndgameResults *endgame_results = config_get_endgame_results(config);

  // Create error stack
  ErrorStack *error_stack = error_stack_create();

  // Print the game position and ply count
  StringBuilder *game_sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, game_sb);
  printf("\n%s\n", string_builder_peek(game_sb));
  string_builder_destroy(game_sb);
  game_string_options_destroy(gso);

  // Create timeout thread if timeout is nonzero
  cpthread_t timeout_thread_id;
  TimeoutThreadArgs timeout_args = {.config = config, .timeout = timeout};
  if (timeout > 0) {
    cpthread_create(&timeout_thread_id, timeout_thread_function, &timeout_args);
  }

  printf("Solving %d-ply endgame with %d threads...\n", endgame_args.plies,
         endgame_args.num_threads);
  endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);

  // Join the timeout thread if it was created
  if (timeout > 0) {
    cpthread_join(timeout_thread_id);
  }

  const error_code_t actual_error_code = error_stack_top(error_stack);
  const bool has_expected_error = actual_error_code == expected_error_code;
  if (!has_expected_error) {
    printf("encountered expected error code in endgame test\nexpected: "
           "%d\nactual:   %d",
           expected_error_code, actual_error_code);
    error_stack_print_and_reset(error_stack);
    assert(0);
  }

  if (actual_error_code == ERROR_STATUS_SUCCESS && timeout == 0) {
    const PVLine *pv_line =
        endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
    assert(pv_line->score == expected_score);
    assert(small_move_is_pass(&pv_line->moves[0]) == is_pass);
    printf(
        "Endgame solved successfully with expected score %d and is_pass=%d\n",
        expected_score, is_pass);
  }

  char *endgame_string =
      endgame_results_get_string(endgame_results, game, NULL);
  printf("\nFinal Endgame Outputs:\n%s\n", endgame_string);
  free(endgame_string);

  endgame_ctx_destroy(endgame_ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_vs_joey(void) {
  /*
  This one is kind of tricky! You need to look at least 13 plies deep to
  find the right sequence, even though it is only 7 moves long:

  Best sequence has a spread difference (value) of +55
  Final spread after seq: +14
  Best sequence:
  1) J11 .R (2)
  2)  F2 DI. (19)
  3)  6N .I (11)
  4) (Pass) (0)
  5) 12I E. (4)
  6) (Pass) (0)
  7) H12 FLAM (49)
  */

  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 13",
      "cgp "
      "AIDER2U7/b1E1E2N1Z5/AWN1T2M1ATT3/LI1COBLE2OW3/OP2U2E2AA3/NE2CUSTARDS1Q1/"
      "ER1OH5I2U1/S2K2FOB1ERGOT/5HEXYLS2I1/4JIN6N1/2GOOP2NAIVEsT/1DIRE10/"
      "2GAY10/15/15 AEFILMR/DIV 371/412 0 -lex NWL20;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 55, false,
      0);
}

void test_pass_first(void) {
  // This endgame's first move must be a pass, otherwise Nigel can set up
  // an unblockable ZA. Exact value is -63 (verified at 25-ply with and
  // without heuristics). The optimal PV is 10 moves with 4 passes, so
  // 7-ply relies on greedy playout for the tail.
  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 7",
      "cgp "
      "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/"
      "1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/"
      "3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW21",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, -63, true,
      0);
}

void test_nonempty_bag(void) {
  // The solver should return an error if the bag is not empty.
  test_single_endgame("set -s1 score -s2 score -threads 6 -eplies 4",
                      "cgp " EMPTY_CGP, DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                      ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY, 0, false, 0);
}

void test_single_pv_display(void) {
  // Solve a multi-PV endgame, then verify that shendgame <n> produces output
  // via string_builder_endgame_single_pv for each valid PV index .
  Config *config =
      config_create_or_die("set -s1 score -s2 score -eplies 4 -etopk 5");
  load_and_exec_config_or_die(
      config, "cgp "
              "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
              "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
              "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20");

  Game *game = config_get_game(config);
  EndgameResults *endgame_results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();
  EndgameCtx *endgame_ctx = NULL;

  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = config_get_endgame_plies(config);
  endgame_args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 6;
  endgame_args.use_heuristics = true;
  endgame_args.forced_pass_bypass = true;
  endgame_args.enable_pv_display = true;
  endgame_args.num_top_moves = 5;
  endgame_args.seed = 42;

  endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  const int num_pvs = endgame_results_get_num_pvs(endgame_results);
  assert(num_pvs > 1);

  const Game *source_game = endgame_results_get_start_game(endgame_results);

  // Each valid 0-indexed PV should produce non-empty output.
  for (int pv_idx = 0; pv_idx < num_pvs; pv_idx++) {
    StringBuilder *sb = string_builder_create();
    string_builder_endgame_single_pv(sb, endgame_results, source_game, NULL,
                                     pv_idx);
    const char *output = string_builder_peek(sb);
    assert(output && output[0] != '\0');
    printf("single_pv display %d/%d:\n%s\n", pv_idx + 1, num_pvs, output);
    string_builder_destroy(sb);
  }

  endgame_ctx_destroy(endgame_ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_ctx_reuse(void) {
  // Solve the same position multiple times with the same EndgameCtx, varying
  // the thread count each solve to exercise worker-pool growth/reuse and
  // the thread_capacity / worker_ids allocation paths.
  const char *cgp =
      "cgp "
      "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
      "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
      "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20";

  Config *config = config_create_or_die("set -s1 score -s2 score -eplies 4");
  load_and_exec_config_or_die(config, cgp);

  Game *game = config_get_game(config);
  EndgameResults *endgame_results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();
  EndgameCtx *endgame_ctx = NULL;

  const int thread_counts[] = {1, 3, 6, 2, 1, 4};
  const int num_solves = 6;
  for (int solve_idx = 0; solve_idx < num_solves; solve_idx++) {
    EndgameArgs endgame_args = {0};
    endgame_args.thread_control = config_get_thread_control(config);
    endgame_args.game = game;
    endgame_args.plies = config_get_endgame_plies(config);
    endgame_args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
    endgame_args.initial_small_move_arena_size =
        DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
    endgame_args.num_threads = thread_counts[solve_idx];
    endgame_args.use_heuristics = true;
    endgame_args.forced_pass_bypass = true;
    endgame_args.enable_pv_display = true;
    endgame_args.num_top_moves = 1;
    endgame_args.seed = 42;

    printf("ctx reuse solve %d/%d: %d thread(s)\n", solve_idx + 1, num_solves,
           endgame_args.num_threads);
    endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);
    assert(error_stack_is_empty(error_stack));

    const PVLine *pv_line =
        endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
    assert(pv_line->score == 11);
    assert(!small_move_is_pass(&pv_line->moves[0]));
  }

  endgame_ctx_destroy(endgame_ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_solve_standard(void) {
  // A standard out-in-two endgame.
  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 4",
      "cgp "
      "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
      "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
      "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 11, false,
      0);
}

void test_very_deep(void) {
  // This insane endgame requires 25 plies to solve. We end up winning by 1 pt.
  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 25",
      "cgp "
      "14C/13QI/12FIE/10VEE1R/9KIT2G/8CIG1IDE/8UTA2AS/7ST1SYPh1/6JA5A1/"
      "5WOLD2BOBA/3PLOT1R1NU1EX/Y1VEIN1NOR1mOA1/UT1AT1N1L2FEH1/"
      "GUR2WIRER5/SNEEZED8 ADENOOO/AHIILMM 353/236 0 -lex CSW21;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, -116, true,
      0);
}

void test_eldar_v_stick(void) {
  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 3",
      "cgp "
      "4EXODE6/1DOFF1KERATIN1U/1OHO8YEN/1POOJA1B3MEWS/5SQUINTY2A/4RHINO1e3V/"
      "2B4C2R3E/GOAT1D1E2ZIN1d/1URACILS2E4/1PIG1S4T4/2L2R4T4/2L2A1GENII3/"
      "2A2T1L7/5E1A7/5D1M7 AEEIRUW/V 410/409 0 -lex CSW21;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 72, false,
      0);
}

void test_small_arena_realloc(void) {
  test_single_endgame(
      "set -s1 score -s2 score -threads 6 -eplies 4",
      "cgp "
      "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
      "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
      "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20",
      512, ERROR_STATUS_SUCCESS, 11, false, 0);
}

void test_endgame_interrupt(void) {
  test_single_endgame(
      "set -s1 score -s2 score -threads 1 -eplies 25 -etopk 10",
      "cgp "
      "4EXODE6/1DOFF1KERATIN1U/1OHO8YEN/1POOJA1B3MEWS/5SQUINTY2A/4RHINO1e3V/"
      "2B4C2R3E/GOAT1D1E2ZIN1d/1URACILS2E4/1PIG1S4T4/2L2R4T4/2L2A1GENII3/"
      "2A2T1L7/5E1A7/5D1M7 AEEIRUW/V 410/409 0 -lex CSW21;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 0, true, 3);
}

void test_kue(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score -eplies 14 "
                                        "-ttfraction 0.5");
  load_and_exec_config_or_die(
      config, "cgp "
              "6MOO1VIRLS/1EJECTA6A1/2I2AEON4R1/2BAH6X1N1/2SLID4GIFTS/"
              "4DONG1OR1R1i/7HOURLY1Z/FE4DINT1A2Y/RECLINE2I1N3/"
              "EW1ATAP2E1G3/M10U3/D3PATOOTIE3/15/15/15 "
              "?AEEKSU/BEIQUVW 276/321 0 -lex NWL23;");

  EndgameCtx *endgame_ctx = NULL;
  Game *game = config_get_game(config);
  Timer timer;
  ctimer_start(&timer);

  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = 14;
  endgame_args.tt_fraction_of_mem = 0.5;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 8;
  endgame_args.use_heuristics = true;
  endgame_args.num_top_moves = 10;
  endgame_args.per_ply_callback = print_pv_and_ranked_callback;
  endgame_args.per_ply_callback_data = &timer;
  endgame_args.seed = 42;

  EndgameResults *endgame_results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();

  StringBuilder *game_sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, game_sb);
  printf("\n%s\n", string_builder_peek(game_sb));
  string_builder_destroy(game_sb);
  game_string_options_destroy(gso);

  printf("Solving %d-ply endgame with %d threads, top %d moves, "
         "ttfraction=%.1f...\n",
         endgame_args.plies, endgame_args.num_threads,
         endgame_args.num_top_moves, endgame_args.tt_fraction_of_mem);
  endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  const PVLine *pv_line =
      endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
  assert(pv_line->score == 52);

  endgame_ctx_destroy(endgame_ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// QI-relevant 2-lexicon endgame test.
// TWL98 vs CSW24 - TWL98 doesn't have QI, CSW24 does.
// Position from TWL98 vs CSW24 game (seed 1023).
// Player 1 (TWL98) has IQV - has Q and I but can't play QI in TWL98.
// CSW24 player on turn.
#define TWO_LEXICON_CGP                                                        \
  "cgp "                                                                       \
  "DOBIE2ARCSINES/1FANWORT4OX1/7O3FROM/7K3L3/6VEEJAY3/11MOA1/"                 \
  "7PIgWEEDS/5DUI3N2H/5ETUI4TI/3GUP7AL/13NY/4TItHONIA1G1/"                     \
  "7E5L1/4RECRATE2E1/7D1ANGORA BELSTUZ/IQV 373/426 0"

void test_2lex_endgame(dual_lexicon_mode_t mode, int expected_score) {
  Config *config = config_create_or_die(
      "set -l1 TWL98 -l2 CSW24 -wmp false -s1 score -s2 score"
      " -threads 1 -eplies 4");
  load_and_exec_config_or_die(config, TWO_LEXICON_CGP);

  EndgameCtx *solver = NULL;
  Game *game = config_get_game(config);
  ErrorStack *error_stack = error_stack_create();
  EndgameResults *results = config_get_endgame_results(config);

  EndgameArgs args = {
      .thread_control = config_get_thread_control(config),
      .game = game,
      .plies = 3,
      .tt_fraction_of_mem = 0.05,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .num_top_moves = 1,
      .use_heuristics = false,
      .per_ply_callback = NULL,
      .per_ply_callback_data = NULL,
      .dual_lexicon_mode = mode,
      .seed = 42,
  };

  endgame_solve(&solver, &args, results, error_stack);
  assert(error_stack_is_empty(error_stack));
  const PVLine *pv_line =
      endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  assert(pv_line->score == expected_score);

  error_stack_destroy(error_stack);
  endgame_ctx_destroy(solver);
  config_destroy(config);
}

void test_2lex_ignorant(void) {
  test_2lex_endgame(DUAL_LEXICON_MODE_IGNORANT, 96);
}

void test_2lex_informed(void) {
  test_2lex_endgame(DUAL_LEXICON_MODE_INFORMED, 81);
}

// Test that topk_fully_solved fires when the actual game is shorter than eplies
// (all branches end before the depth limit), and does NOT fire when unfinished
// leaves remain. Covers single-thread, multi-thread, single-PV, and etopk.
void test_topk_fully_solved(void) {
  // --- Case 1: game ends before eplies (early-stop must fire) ---
  // BGIV/DEHILOR: 4+7=11 tiles. Score=11, confirmed by test_solve_standard
  // at eplies=4. With eplies=10, early-stop fires at depth 7-8 (< 10),
  // well before the depth limit. The search completes in seconds even
  // single-threaded because the 11-tile game tree is small.
  const char *short_cgp =
      "cgp 9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
      "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
      "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20";

  typedef struct {
    int threads;
    int topk;
  } EarlyStopCase;
  const EarlyStopCase cases[] = {
      {1, 1}, // single-thread, single-PV
      {6, 1}, // multi-thread ABDADA, single-PV
      {6, 3}, // multi-thread ABDADA, etopk
  };
  for (int ci = 0; ci < 3; ci++) {
    Config *config = config_create_or_die("set -s1 score -s2 score");
    load_and_exec_config_or_die(config, short_cgp);

    EndgameCtx *ctx = NULL;
    EndgameArgs args = {0};
    args.thread_control = config_get_thread_control(config);
    args.game = config_get_game(config);
    args.plies = 10;
    args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
    args.initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
    args.num_threads = cases[ci].threads;
    args.num_top_moves = cases[ci].topk;
    args.use_heuristics = true;
    args.forced_pass_bypass = true;
    args.seed = 42;

    EndgameResults *results = config_get_endgame_results(config);
    ErrorStack *error_stack = error_stack_create();
    endgame_solve(&ctx, &args, results, error_stack);
    assert(error_stack_is_empty(error_stack));

    const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
    int completed_depth =
        endgame_results_get_depth(results, ENDGAME_RESULT_BEST);
    printf("topk_fully_solved early-stop (threads=%d topk=%d): "
           "score=%d completed_depth=%d\n",
           cases[ci].threads, cases[ci].topk, pv->score, completed_depth);
    // Correct score: 11 (matches test_solve_standard at eplies=4).
    assert(pv->score == 11);
    // Early-stop must have fired (completed_depth < eplies=10).
    assert(completed_depth > 0 && completed_depth < 10);

    endgame_ctx_destroy(ctx);
    error_stack_destroy(error_stack);
    config_destroy(config);
  }

  // --- Case 2: non-terminal horizon (early-stop must NOT fire) ---
  // ?AEEKSU/BEIQUVW: 14 tiles. At eplies=2 most branches still have tiles
  // in hand → any_leaf_game_unfinished is set → topk_fully_solved stays false.
  // The solver must terminate via ply==plies, so completed_depth == 2.
  {
    Config *config =
        config_create_or_die("set -s1 score -s2 score -ttfraction 0.5");
    load_and_exec_config_or_die(
        config, "cgp 6MOO1VIRLS/1EJECTA6A1/2I2AEON4R1/2BAH6X1N1/2SLID4GIFTS/"
                "4DONG1OR1R1i/7HOURLY1Z/FE4DINT1A2Y/RECLINE2I1N3/"
                "EW1ATAP2E1G3/M10U3/D3PATOOTIE3/15/15/15 "
                "?AEEKSU/BEIQUVW 276/321 0 -lex NWL23;");

    EndgameCtx *ctx = NULL;
    EndgameArgs args = {0};
    args.thread_control = config_get_thread_control(config);
    args.game = config_get_game(config);
    args.plies = 2;
    args.tt_fraction_of_mem = 0.5;
    args.initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
    args.num_threads = 6;
    args.num_top_moves = 3;
    args.use_heuristics = false;
    args.seed = 42;

    EndgameResults *results = config_get_endgame_results(config);
    ErrorStack *error_stack = error_stack_create();
    endgame_solve(&ctx, &args, results, error_stack);
    assert(error_stack_is_empty(error_stack));

    int completed_depth =
        endgame_results_get_depth(results, ENDGAME_RESULT_BEST);
    printf("topk_fully_solved non-terminal horizon: completed_depth=%d\n",
           completed_depth);
    // Early-stop must NOT have fired; solver must reach the eplies limit.
    assert(completed_depth == 2);

    endgame_ctx_destroy(ctx);
    error_stack_destroy(error_stack);
    config_destroy(config);
  }
}

void test_endgame(void) {
  test_single_pv_display();
  test_ctx_reuse();
  test_solve_standard();
  test_very_deep();
  test_small_arena_realloc();
  test_pass_first();
  test_nonempty_bag();
  // 2-lexicon endgame tests (TWL98 vs CSW24, QI-relevant)
  test_2lex_ignorant();
  test_2lex_informed();
  test_endgame_interrupt();
  test_topk_fully_solved();
  //  Uncomment out more of these tests once we add more optimizations,
  //  and/or if we can run the endgame tests in release mode.
  // test_vs_joey();
}

void test_monster_q(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score -eplies 6 "
                                        "-ttfraction 0.5");
  load_and_exec_config_or_die(config,
                              "cgp "
                              "5LEX1AFFORD/3SNOWIER1Y3/2V8T3/1DO6J1T3/1AG6OKE3/"
                              "1U7YE3N/1B8T3E/ERICA2GARTH2V/1I2MIAOW1L3U/"
                              "PE6AZIONES/OS8n4/L6DINGBATS/I14/COULEE9/"
                              "E3HARN7 "
                              "ADEIIU?/MNPQRT 369/399 0 -lex CSW21;");

  EndgameCtx *endgame_ctx = NULL;
  Game *game = config_get_game(config);
  Timer timer;
  ctimer_start(&timer);

  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = 6;
  endgame_args.tt_fraction_of_mem = 0.5;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 6;
  endgame_args.use_heuristics = true;
  endgame_args.forced_pass_bypass = true;
  endgame_args.enable_pv_display = true;
  endgame_args.num_top_moves = 1;
  endgame_args.per_ply_callback = print_pv_callback;
  endgame_args.per_ply_callback_data = &timer;
  endgame_args.seed = 42;

  EndgameResults *endgame_results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();

  StringBuilder *game_sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, game_sb);
  printf("\n%s\n", string_builder_peek(game_sb));
  string_builder_destroy(game_sb);
  game_string_options_destroy(gso);

  printf("Solving %d-ply endgame with %d threads, ttfraction=%.1f...\n",
         endgame_args.plies, endgame_args.num_threads,
         endgame_args.tt_fraction_of_mem);
  endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  endgame_ctx_destroy(endgame_ctx);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_multi_pv(void) {
  // Test multi-PV mode: solve a 4-ply endgame requesting top 5 moves.
  // Verify we get multiple PVs back with values in descending order,
  // and the best PV matches the single-PV result.
  Config *config = config_create_or_die("set -s1 score -s2 score -eplies 4");
  load_and_exec_config_or_die(
      config, "cgp "
              "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
              "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
              "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20");

  // First: solve single-PV to get the reference best value
  EndgameCtx *endgame_ctx = NULL;
  Game *game = config_get_game(config);

  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = 4;
  endgame_args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 6;
  endgame_args.num_top_moves = 1;
  endgame_args.use_heuristics = true;
  endgame_args.per_ply_callback = NULL;
  endgame_args.per_ply_callback_data = NULL;
  endgame_args.seed = 42;

  EndgameResults *endgame_results = endgame_results_create();
  ErrorStack *error_stack = error_stack_create();

  endgame_solve(&endgame_ctx, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));
  const PVLine *single_pv =
      endgame_results_get_pvline(endgame_results, ENDGAME_RESULT_BEST);
  int32_t single_best_score = single_pv->score;

  endgame_ctx_destroy(endgame_ctx);

  // Now: solve multi-PV with top 5
  // Multi-PV ranked moves are reported via the per-ply callback;
  // the results struct stores only the best PV.
  endgame_ctx = NULL;
  endgame_args.num_top_moves = 5;
  Timer timer;
  ctimer_start(&timer);
  endgame_args.per_ply_callback = print_pv_and_ranked_callback;
  endgame_args.per_ply_callback_data = &timer;

  StringBuilder *game_sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, game_sb);
  printf("\n%s\n", string_builder_peek(game_sb));
  string_builder_destroy(game_sb);
  game_string_options_destroy(gso);

  printf("Solving %d-ply endgame with %d threads, top %d moves...\n",
         endgame_args.plies, endgame_args.num_threads,
         endgame_args.num_top_moves);

  EndgameResults *multi_results = endgame_results_create();
  endgame_solve(&endgame_ctx, &endgame_args, multi_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  // Best PV should match single-PV result
  const PVLine *best_pv =
      endgame_results_get_pvline(multi_results, ENDGAME_RESULT_BEST);
  printf("Single-PV best: %d, Multi-PV best: %d\n", single_best_score,
         best_pv->score);
  assert(best_pv->score == single_best_score);
  assert(best_pv->num_moves >= 1);

  // Test string output
  char *result_str = endgame_results_get_string(multi_results, game, NULL);
  printf("Multi-PV output:\n%s\n", result_str);
  free(result_str);

  endgame_ctx_destroy(endgame_ctx);
  endgame_results_destroy(multi_results);
  endgame_results_destroy(endgame_results);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_endgame_wasm(void) {
  test_solve_standard();
  test_small_arena_realloc();
  test_pass_first();
  test_nonempty_bag();
}
