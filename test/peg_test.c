#include "peg_test.h"

#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void print_game_position(const Game *game) {
  StringBuilder *sb = string_builder_create();
  GameStringOptions *gso = game_string_options_create_default();
  string_builder_add_game(game, NULL, gso, NULL, sb);
  printf("\n%s\n", string_builder_peek(sb));
  string_builder_destroy(sb);
  game_string_options_destroy(gso);
}

static void peg_progress_callback(int pass, int num_evaluated,
                                   const SmallMove *top_moves,
                                   const double *top_values,
                                   const double *top_win_pcts, int num_top,
                                   const Game *game, double elapsed,
                                   double stage_seconds, void *user_data) {
  (void)user_data;
  if (pass == 0) {
    printf("[PEG greedy] %d candidates evaluated in %.3fs\n", num_evaluated,
           stage_seconds);
  } else {
    printf("[PEG %d-ply endgame] %d candidates evaluated in %.3fs (%.3fs "
           "cumulative)\n",
           pass, num_evaluated, stage_seconds, elapsed);
  }
  const LetterDistribution *ld = game_get_ld(game);
  int mover_idx = game_get_player_on_turn_index(game);
  const Rack *mover_rack = player_get_rack(game_get_player(game, mover_idx));
  for (int i = 0; i < num_top; i++) {
    Move m;
    small_move_to_move(&m, &top_moves[i], game_get_board(game));
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), &m, ld, false);
    int score = (int)small_move_get_score(&top_moves[i]);
    // Compute leave: mover's rack minus the tiles played.
    Rack leave;
    rack_copy(&leave, mover_rack);
    for (int j = 0; j < m.tiles_length; j++) {
      MachineLetter ml = m.tiles[j];
      if (ml != PLAYED_THROUGH_MARKER)
        rack_take_letter(&leave, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
    }
    string_builder_add_formatted_string(sb, " %d  leave=", score);
    string_builder_add_rack(sb, &leave, ld, false);
    printf("  %d. %s  win%%=%.1f%%  spread=%+.2f\n", i + 1,
           string_builder_peek(sb), top_win_pcts[i] * 100.0, top_values[i]);
    string_builder_destroy(sb);
  }
}

static void print_peg_result(const PegResult *result, const Game *game) {
  Move m;
  small_move_to_move(&m, &result->best_move, game_get_board(game));
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), &m, game_get_ld(game),
                          false);
  const char *depth_label =
      result->passes_completed == 0 ? "greedy" : "endgame";
  printf("  Best move: %s  win%%=%.1f%%  spread=%.2f  depth=%d (%s)\n",
         string_builder_peek(sb), result->best_win_pct * 100.0,
         result->best_expected_spread, result->passes_completed, depth_label);
  string_builder_destroy(sb);
}

// From macondo TestStraightforward1PEG (NWL20).
// Mover has ENOSTXY, 1 tile in bag (8 unseen total: 1 bag + 7 opponent tiles).
static void test_peg_straightforward(void) {
  Config *config =
      config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config,
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/"
      "E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/"
      "1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex "
      "NWL20");

  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 1);
  print_game_position(game);

  PegSolver *solver = peg_solver_create();
  PegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .time_budget_seconds = 0.0,
      .num_threads = 1,
      .tt_fraction_of_mem = 0.5,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .num_passes = 2,
      .pass_candidate_limits = {32, 16},
      .per_pass_callback = peg_progress_callback,
      .per_pass_num_top = 20,
  };

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  peg_solve(solver, &args, &result, error_stack);

  assert(error_stack_is_empty(error_stack));
  assert(result.passes_completed == 2);
  assert(!small_move_is_pass(&result.best_move));
  {
    Move best;
    small_move_to_move(&best, &result.best_move, game_get_board(game));
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), &best,
                            game_get_ld(game), false);
    printf("  Best move string: %s\n", string_builder_peek(sb));
    assert(strcmp(string_builder_peek(sb), "13L ONYX") == 0);
    string_builder_destroy(sb);
  }
  assert(result.best_win_pct > 0.93 && result.best_win_pct < 0.94);
  assert(result.best_expected_spread > 0.0);
  print_peg_result(&result, game);

  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Same position as test_peg_straightforward but with multiple threads.
// Verifies that multi-threaded execution produces the same best move.
static void test_peg_multithreaded(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config,
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/"
      "E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/"
      "1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex "
      "NWL20");

  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 1);

  PegSolver *solver = peg_solver_create();
  PegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .time_budget_seconds = 0.0,
      .num_threads = 4,
      .tt_fraction_of_mem = 0.5,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .num_passes = 2,
      .pass_candidate_limits = {32, 16},
      .per_pass_callback = peg_progress_callback,
      .per_pass_num_top = 5,
  };

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  peg_solve(solver, &args, &result, error_stack);

  assert(error_stack_is_empty(error_stack));
  assert(result.passes_completed == 2);
  assert(!small_move_is_pass(&result.best_move));
  {
    Move best;
    small_move_to_move(&best, &result.best_move, game_get_board(game));
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), &best,
                            game_get_ld(game), false);
    printf("  [MT] Best move: %s\n", string_builder_peek(sb));
    assert(strcmp(string_builder_peek(sb), "13L ONYX") == 0);
    string_builder_destroy(sb);
  }
  assert(result.best_win_pct > 0.93 && result.best_win_pct < 0.94);
  print_peg_result(&result, game);

  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Verify that time_budget_seconds causes early termination.
// Requests many passes but sets a tight time budget so not all complete.
static void test_peg_time_budget(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config,
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/"
      "E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/"
      "1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex "
      "NWL20");

  Game *game = config_get_game(config);

  PegSolver *solver = peg_solver_create();
  // Request 10 endgame passes but with a 5s budget on 4 threads.
  // The greedy pass alone takes ~1.5s single-threaded, so with 4 threads
  // and a 5s budget we should complete greedy + a few endgame passes
  // but not all 10.
  PegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .time_budget_seconds = 5.0,
      .num_threads = 4,
      .tt_fraction_of_mem = 0.5,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .num_passes = 10,
      .pass_candidate_limits = {32, 16, 8, 8, 4, 4, 4, 4, 4, 4},
      .per_pass_callback = peg_progress_callback,
      .per_pass_num_top = 3,
  };

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  peg_solve(solver, &args, &result, error_stack);

  assert(error_stack_is_empty(error_stack));
  // The greedy pass (pass 0) always runs. Under sanitizers or slow machines,
  // we may not complete any endgame passes within the time budget, so we only
  // assert that we didn't complete all requested passes.
  printf("  [TimeBudget] passes_completed=%d (requested 10)\n",
         result.passes_completed);
  assert(result.passes_completed < 10);
  assert(!small_move_is_pass(&result.best_move));
  print_peg_result(&result, game);

  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Verify that peg_solve rejects a position with 0 unseen tiles.
static void test_peg_no_unseen_error(void) {
  Config *config =
      config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config,
      "cgp 9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/"
      "8WE2OBI/6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/"
      "FIVE1E5IT1C/5SPORRAN2A/6ORE2N2D / 384/389 0 -lex NWL20");

  Game *game = config_get_game(config);

  PegSolver *solver = peg_solver_create();
  PegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
  };

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  peg_solve(solver, &args, &result, error_stack);

  assert(!error_stack_is_empty(error_stack));
  assert(error_stack_top(error_stack) == ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY);

  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Same position, 4 threads, greedy only (no endgame passes).
static void test_peg_mt_greedy_only(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config,
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/"
      "E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/"
      "1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex "
      "NWL20");

  PegSolver *solver = peg_solver_create();
  PegArgs args = {
      .game = config_get_game(config),
      .thread_control = config_get_thread_control(config),
      .num_threads = 4,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .num_passes = 0,
      .per_pass_callback = peg_progress_callback,
      .per_pass_num_top = 3,
  };

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  peg_solve(solver, &args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(result.passes_completed == 0);
  printf("  [MT greedy] Best move found OK\n");
  print_peg_result(&result, config_get_game(config));

  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_peg(void) {
  test_peg_mt_greedy_only();
  test_peg_multithreaded();
  test_peg_time_budget();
  test_peg_straightforward();
  test_peg_no_unseen_error();
}
