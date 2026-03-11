#include "peg2_test.h"

#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "peg_test_util.h"
#include "test_util.h"

#include <assert.h>
#include <math.h>
#include <string.h>

// The CSW21 2-in-bag CGP used by all peg2 tests.
#define PEG2_CGP                                                               \
  "cgp 1T13/1W3Q9/VERB1U9/1E1OPIUM5C1/1LAWIN1I5O1/1Y3A1E5R1/"               \
  "7V4NO1/NOTArIZE1C2UN1/6ODAH2LA1/3TAHA2I2LED/2JUT4R2A1O/"                  \
  "3G5P4D/3R3BrIEFING/3I5L4E/3K2DESYNES1M AEFGSTX/EEIOOST "                  \
  "370/341 0 -lex CSW21"

// From macondo TestTwoInBagSingleMove (CSW21, originally CSW19).
// Mover has AEFGSTX, 2 tiles in bag (9 unseen total: 2 bag + 7 opponent tiles).
// The winning move is 6F .X. (play X through the existing A on 6F), which wins
// 70 out of 72 ordered-pair scenarios.  It only loses when the bag contains
// (E,I) and mover draws I: opponent bingoes with TOREROS/ROOSTER.
static void test_peg_two_in_bag(void) {
  Config *config =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config, PEG2_CGP);

  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 2);
  peg_test_print_game_position(game);

  PegSolver *solver = peg_solver_create();
  PegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .time_budget_seconds = 0.0,
      .num_threads = 8,
      .tt_fraction_of_mem = 0.5,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .num_stages = 1,
      .stage_candidate_limits = {},
      .early_cutoff = true,
      .inner_opp_multi_tile_limit = 8,
      .inner_opp_one_tile_limit = 3,
      .skip_phase_1b = false,
      .skip_root_pass = true,
      .per_pass_callback = peg_test_progress_callback,
      .per_pass_num_top = 128,
      .first_win_mode = PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,
  };

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  peg_solve(solver, &args, &result, error_stack);

  assert(error_stack_is_empty(error_stack));
  assert(move_get_type(&result.best_move) != GAME_EVENT_PASS);
  {
    Move best = result.best_move;
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), &best,
                            game_get_ld(game), false);
    assert(strcmp(string_builder_peek(sb), "10I X(I)") == 0);
    string_builder_destroy(sb);
  }
  assert(fabs(result.best_win_pct - 70.0 / 72.0) < 0.005);
  assert(fabs(result.best_expected_spread - 46.86) < 0.5);
  peg_test_print_result(&result, game);

  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Focused test: evaluate only X(I) (1-tile non-emptying), EXT(R)AS (6-tile
// bag-emptying), and S(NO)T (2-tile bag-emptying) to exercise all three phases
// without the cost of evaluating 700+ candidates.
static void test_peg2_xi_extras_snot_impl(void) {
  Config *config =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config, PEG2_CGP);

  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 2);
  peg_test_print_game_position(game);

  const char *allowlist[] = {"10I X(I)", "13A EXT(R)AS", "7L S(NO)T"};

  PegSolver *solver = peg_solver_create();
  PegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .time_budget_seconds = 0.0,
      .num_threads = 8,
      .tt_fraction_of_mem = 0.5,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .num_stages = 1,
      .stage_candidate_limits = {},
      .early_cutoff = false,
      .inner_opp_multi_tile_limit = 8,
      .inner_opp_one_tile_limit = 3,
      .max_non_emptying = 3,
      .skip_phase_1b = false,
      .skip_root_pass = true,
      .per_pass_callback = peg_test_progress_callback,
      .per_pass_num_top = 128,
      .candidate_allowlist = allowlist,
      .candidate_allowlist_count = 3,
      .first_win_mode = PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,
  };

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  peg_solve(solver, &args, &result, error_stack);

  assert(error_stack_is_empty(error_stack));
  // X(I) should still be best among these three.
  {
    Move best = result.best_move;
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), &best,
                            game_get_ld(game), false);
    assert(strcmp(string_builder_peek(sb), "10I X(I)") == 0);
    string_builder_destroy(sb);
  }
  assert(fabs(result.best_win_pct - 70.0 / 72.0) < 0.005);
  peg_test_print_result(&result, game);

  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_peg2(void) {
  test_peg_two_in_bag();
}

void test_peg2_xi_extras_snot(void) {
  test_peg2_xi_extras_snot_impl();
}
