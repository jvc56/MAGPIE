#include "../src/compat/ctime.h"
#include "../src/ent/board.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/str/rack_string.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

// Per-ply callback to print PV during iterative deepening
static void print_pv_callback(int depth, int32_t value, const PVLine *pv_line,
                              const Game *game, void *user_data) {
  const Timer *timer = (const Timer *)user_data;
  double elapsed = ctimer_elapsed_seconds(timer);

  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb, "  depth %d: value=%d, time=%.3fs, pv=", depth, value, elapsed);

  // Format each move in the PV
  Game *gc = game_duplicate(game);
  const Board *board = game_get_board(gc);
  const LetterDistribution *ld = game_get_ld(gc);
  Move move;

  for (int i = 0; i < pv_line->num_moves; i++) {
    small_move_to_move(&move, &(pv_line->moves[i]), board);
    string_builder_add_move(sb, board, &move, ld, true);
    play_move(&move, gc, NULL);
    if (game_get_game_end_reason(gc) == GAME_END_REASON_STANDARD) {
      // Player went out: show opponent rack tiles and +2x rack points
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
      string_builder_add_string(sb, " ");
    }
  }

  // Compute final spread: value is from root player's perspective
  int on_turn = game_get_player_on_turn_index(game);
  int p1 = equity_to_int(player_get_score(game_get_player(game, 0)));
  int p2 = equity_to_int(player_get_score(game_get_player(game, 1)));
  int final_spread = (p1 - p2) + (on_turn == 0 ? value : -value);
  if (final_spread > 0) {
    string_builder_add_formatted_string(sb, " [P1 wins by %d]", final_spread);
  } else if (final_spread < 0) {
    string_builder_add_formatted_string(sb, " [P2 wins by %d]", -final_spread);
  } else {
    string_builder_add_string(sb, " [Tie]");
  }

  fprintf(stderr, "%s\n", string_builder_peek(sb));
  string_builder_destroy(sb);
  game_destroy(gc);
}

void test_single_endgame(const char *config_settings, const char *cgp,
                         int initial_small_move_arena_size,
                         error_code_t expected_error_code,
                         const int expected_score, const bool is_pass) {
  // Load config (endgame tests don't need WMP)
  char settings_buf[512];
  snprintf(settings_buf, sizeof(settings_buf), "%s -wmp false",
           config_settings);
  Config *config = config_create_or_die(settings_buf);
  load_and_exec_config_or_die(config, cgp);

  // Create solver
  EndgameSolver *endgame_solver = endgame_solver_create();

  // Create args
  Game *game = config_get_game(config);
  Timer timer;
  ctimer_start(&timer);
  EndgameArgs endgame_args;
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = config_get_endgame_plies(config);
  endgame_args.tt_fraction_of_mem = config_get_tt_fraction_of_mem(config);
  endgame_args.initial_small_move_arena_size = initial_small_move_arena_size;
  endgame_args.num_threads = 6;
  endgame_args.per_ply_callback = print_pv_callback;
  endgame_args.per_ply_callback_data = &timer;

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

  printf("Solving %d-ply endgame...\n", endgame_args.plies);
  endgame_solve(endgame_solver, &endgame_args, endgame_results, error_stack);

  const error_code_t actual_error_code = error_stack_top(error_stack);
  const bool has_expected_error = actual_error_code == expected_error_code;
  if (!has_expected_error) {
    printf("encountered expected error code in endgame test\nexpected: "
           "%d\nactual:   %d",
           expected_error_code, actual_error_code);
    error_stack_print_and_reset(error_stack);
    assert(0);
  }

  if (actual_error_code == ERROR_STATUS_SUCCESS) {
    const PVLine *pv_line = endgame_results_get_pvline(endgame_results);
    assert(pv_line->score == expected_score);
    assert(small_move_is_pass(&pv_line->moves[0]) == is_pass);
  }

  endgame_solver_destroy(endgame_solver);
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
      "set -s1 score -s2 score -r1 small -r2 small -threads 6 -eplies 13",
      "cgp "
      "AIDER2U7/b1E1E2N1Z5/AWN1T2M1ATT3/LI1COBLE2OW3/OP2U2E2AA3/NE2CUSTARDS1Q1/"
      "ER1OH5I2U1/S2K2FOB1ERGOT/5HEXYLS2I1/4JIN6N1/2GOOP2NAIVEsT/1DIRE10/"
      "2GAY10/15/15 AEFILMR/DIV 371/412 0 -lex NWL20;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 55, false);
}

void test_pass_first(void) {
  // This endgame's first move must be a pass, otherwise Nigel can set up
  // an unblockable ZA.
  test_single_endgame(
      "set -s1 score -s2 score -r1 small -r2 small -threads 6 -eplies 8",
      "cgp "
      "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/"
      "1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/"
      "3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW21",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, -60, true);
}

void test_nonempty_bag(void) {
  // The solver should return an error if the bag is not empty.
  test_single_endgame(
      "set -s1 score -s2 score -r1 small -r2 small -threads 6 -eplies 4",
      "cgp " EMPTY_CGP, DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY, 0, false);
}

void test_solve_standard(void) {
  // A standard out-in-two endgame.
  test_single_endgame(
      "set -s1 score -s2 score -r1 small -r2 small -threads 6 -eplies 4",
      "cgp "
      "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
      "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
      "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 11, false);
}

void test_very_deep(void) {
  // This insane endgame requires 25 plies to solve. We end up winning by 1 pt.
  test_single_endgame(
      "set -s1 score -s2 score -r1 small -r2 small -threads 6 -eplies 25",
      "cgp "
      "14C/13QI/12FIE/10VEE1R/9KIT2G/8CIG1IDE/8UTA2AS/7ST1SYPh1/6JA5A1/"
      "5WOLD2BOBA/3PLOT1R1NU1EX/Y1VEIN1NOR1mOA1/UT1AT1N1L2FEH1/"
      "GUR2WIRER5/SNEEZED8 ADENOOO/AHIILMM 353/236 0 -lex CSW21;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, -116, true);
}

void test_eldar_v_stick(void) {
  test_single_endgame(
      "set -s1 score -s2 score -r1 small -r2 small -threads 6 -eplies 9",
      "cgp "
      "4EXODE6/1DOFF1KERATIN1U/1OHO8YEN/1POOJA1B3MEWS/5SQUINTY2A/4RHINO1e3V/"
      "2B4C2R3E/GOAT1D1E2ZIN1d/1URACILS2E4/1PIG1S4T4/2L2R4T4/2L2A1GENII3/"
      "2A2T1L7/5E1A7/5D1M7 AEEIRUW/V 410/409 0 -lex CSW21;",
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, ERROR_STATUS_SUCCESS, 72, false);
}

void test_small_arena_realloc(void) {
  test_single_endgame(
      "set -s1 score -s2 score -r1 small -r2 small -threads 6 -eplies 4",
      "cgp "
      "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
      "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
      "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20",
      512, ERROR_STATUS_SUCCESS, 11, false);
}

void test_14domino(void) {
  Config *config = config_create_or_die(
      "set -s1 score -s2 score -r1 small -r2 small -wmp false -eplies 14 "
      "-ttfraction 0.5");
  load_and_exec_config_or_die(
      config,
      "cgp "
      "6MOO1VIRLS/1EJECTA6A1/2I2AEON4R1/2BAH6X1N1/2SLID4GIFTS/"
      "4DONG1OR1R1i/7HOURLY1Z/FE4DINT1A2Y/RECLINE2I1N3/"
      "EW1ATAP2E1G3/M10U3/D3PATOOTIE3/15/15/15 "
      "?AEEKSU/BEIQUVW 276/321 0 -lex NWL23;");

  EndgameSolver *endgame_solver = endgame_solver_create();
  Game *game = config_get_game(config);
  Timer timer;
  ctimer_start(&timer);

  EndgameArgs endgame_args;
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = 14;
  endgame_args.tt_fraction_of_mem = 0.5;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 6;
  endgame_args.per_ply_callback = print_pv_callback;
  endgame_args.per_ply_callback_data = &timer;

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
  endgame_solve(endgame_solver, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  const PVLine *pv_line = endgame_results_get_pvline(endgame_results);
  assert(pv_line->score == 52);

  endgame_solver_destroy(endgame_solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_kue14domino(void) {
  // Position after 11K K(U)E (26 pts) then 13D QI (15 pts) from 14domino.
  // P1 to move with ?AESU. Compare to optimal reply VIEW to confirm QI is
  // suboptimal for Player 2.
  Config *config = config_create_or_die(
      "set -s1 score -s2 score -r1 small -r2 small -wmp false -eplies 12 "
      "-ttfraction 0.5");
  load_and_exec_config_or_die(
      config,
      "cgp "
      "6MOO1VIRLS/1EJECTA6A1/2I2AEON4R1/2BAH6X1N1/2SLID4GIFTS/"
      "4DONG1OR1R1i/7HOURLY1Z/FE4DINT1A2Y/RECLINE2I1N3/"
      "EW1ATAP2E1G3/M9KUE2/D3PATOOTIE3/3QI10/15/15 "
      "?AESU/BEUVW 302/336 0 -lex NWL23;");

  EndgameSolver *endgame_solver = endgame_solver_create();
  Game *game = config_get_game(config);
  Timer timer;
  ctimer_start(&timer);

  EndgameArgs endgame_args;
  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = 12;
  endgame_args.tt_fraction_of_mem = 0.5;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 6;
  endgame_args.per_ply_callback = print_pv_callback;
  endgame_args.per_ply_callback_data = &timer;

  EndgameResults *endgame_results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();

  StringBuilder *game_sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, game_sb);
  printf("\n%s\n", string_builder_peek(game_sb));
  string_builder_destroy(game_sb);
  game_string_options_destroy(gso);

  printf("After K(U)E + QI 13D:\n");
  printf("Solving %d-ply endgame with %d threads, ttfraction=%.1f...\n",
         endgame_args.plies, endgame_args.num_threads,
         endgame_args.tt_fraction_of_mem);
  endgame_solve(endgame_solver, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  endgame_solver_destroy(endgame_solver);
  error_stack_destroy(error_stack);
  config_destroy(config);

  // Now solve the same position but with VIEW 7C as the reply instead.
  // VIEW scores 29 (V*2 DL + I + E + W = 14, cross HIDE = 8, cross DOW = 7).
  // P2: 321+29=350, rack BQU.
  config = config_create_or_die(
      "set -s1 score -s2 score -r1 small -r2 small -wmp false -eplies 12 "
      "-ttfraction 0.5");
  load_and_exec_config_or_die(
      config,
      "cgp "
      "6MOO1VIRLS/1EJECTA6A1/2I2AEON4R1/2BAH6X1N1/2SLID4GIFTS/"
      "4DONG1OR1R1i/2VIEW1HOURLY1Z/FE4DINT1A2Y/RECLINE2I1N3/"
      "EW1ATAP2E1G3/M9KUE2/D3PATOOTIE3/15/15/15 "
      "?AESU/BQU 302/350 0 -lex NWL23;");

  endgame_solver = endgame_solver_create();
  game = config_get_game(config);
  ctimer_start(&timer);

  endgame_args.thread_control = config_get_thread_control(config);
  endgame_args.game = game;
  endgame_args.plies = 12;
  endgame_args.tt_fraction_of_mem = 0.5;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 6;
  endgame_args.per_ply_callback = print_pv_callback;
  endgame_args.per_ply_callback_data = &timer;

  endgame_results = config_get_endgame_results(config);
  error_stack = error_stack_create();

  game_sb = string_builder_create();
  gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, game_sb);
  printf("\n%s\n", string_builder_peek(game_sb));
  string_builder_destroy(game_sb);
  game_string_options_destroy(gso);

  printf("After K(U)E + VIEW 7C:\n");
  printf("Solving %d-ply endgame with %d threads, ttfraction=%.1f...\n",
         endgame_args.plies, endgame_args.num_threads,
         endgame_args.tt_fraction_of_mem);
  endgame_solve(endgame_solver, &endgame_args, endgame_results, error_stack);
  assert(error_stack_is_empty(error_stack));

  endgame_solver_destroy(endgame_solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_endgame(void) {
  test_solve_standard();
  test_very_deep();
  test_small_arena_realloc();
  test_pass_first();
  test_nonempty_bag();
  //  Uncomment out more of these tests once we add more optimizations,
  //  and/or if we can run the endgame tests in release mode.
  // test_vs_joey();
  // test_eldar_v_stick();
}

void test_endgame_wasm(void) {
  test_solve_standard();
  test_small_arena_realloc();
  test_pass_first();
  test_nonempty_bag();
}

// Test ground truth value map estimation with Gaussian noise.
// 1. Run a 7-ply search on the 14domino position to record ground truth values
// 2. Re-run the 14-ply search using those values (with noise) as move estimates
// 3. Report nodes searched and time for various noise levels
void test_estimate_quality(void) {
  const char *cgp_str =
      "cgp "
      "6MOO1VIRLS/1EJECTA6A1/2I2AEON4R1/2BAH6X1N1/2SLID4GIFTS/"
      "4DONG1OR1R1i/7HOURLY1Z/FE4DINT1A2Y/RECLINE2I1N3/"
      "EW1ATAP2E1G3/M10U3/D3PATOOTIE3/15/15/15 "
      "?AEEKSU/BEIQUVW 276/321 0 -lex NWL23;";

  // One run: 1 thread, tt_frac=0.25, 7-ply
  double tt_frac = 0.25;
  int nthreads = 16;

  fprintf(stderr,
          "\n=== 7-ply endgame: threads=%d, tt_frac=%.2f ===\n",
          nthreads, tt_frac);

  Config *config = config_create_or_die(
      "set -s1 score -s2 score -r1 small -r2 small -wmp false "
      "-eplies 7 -ttfraction 0.25");
  load_and_exec_config_or_die(config, cgp_str);

  EndgameSolver *solver = endgame_solver_create();
  Game *game = config_get_game(config);

  EndgameArgs solve_args;
  solve_args.thread_control = config_get_thread_control(config);
  solve_args.game = game;
  solve_args.plies = 7;
  solve_args.tt_fraction_of_mem = tt_frac;
  solve_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  solve_args.num_threads = nthreads;
  solve_args.per_ply_callback = print_pv_callback;
  Timer timer;
  ctimer_start(&timer);
  solve_args.per_ply_callback_data = &timer;

  EndgameResults *results = config_get_endgame_results(config);
  ErrorStack *error_stack = error_stack_create();

  endgame_solve(solver, &solve_args, results, error_stack);
  double elapsed = ctimer_elapsed_seconds(&timer);
  assert(error_stack_is_empty(error_stack));

  const PVLine *pv = endgame_results_get_pvline(results);
  fprintf(stderr, "  DONE: score=%d, time=%.3fs\n", pv->score, elapsed);

  error_stack_destroy(error_stack);
  endgame_solver_destroy(solver);
  config_destroy(config);
}