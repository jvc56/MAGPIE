#include "bai_peg_test.h"

#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/impl/bai_peg.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>

// Smoke test: confirms bai_peg_solve runs end-to-end on a 1-in-bag position,
// returns a non-pass move, and produces non-zero evaluation/candidate counts.
// Picking quality at low budget is not asserted (the solver may not converge
// on the truly best move at this budget); the goal here is to catch obvious
// regressions in the search loop, threading, or progressive widening logic.
static void test_bai_peg_smoke(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config, "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/"
              "E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/"
              "1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex "
              "NWL20");

  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 1);

  BaiPegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .num_threads = 1,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .initial_top_k = 32,
      .max_depth = 2,
      .endgame_time_per_solve = 0.5,
      .time_budget_seconds = 30.0,
      .puct_c = 1.0,
  };
  BaiPegResult result;
  ErrorStack *error_stack = error_stack_create();
  bai_peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(!small_move_is_pass(&result.best_move));
  assert(result.evaluations_done > 0);
  assert(result.candidates_considered > 0);
  printf("  bai_peg smoke: evals=%d  best_win%%=%.1f%%  best_spread=%+.2f  "
         "depth=%d  time=%.2fs\n",
         result.evaluations_done, result.best_win_pct * 100.0,
         result.best_mean_spread, result.best_depth_evaluated,
         result.seconds_elapsed);
  error_stack_destroy(error_stack);
  bai_cand_stats_free(result.cand_stats);
  config_destroy(config);
}

// Confirms bai_peg_solve raises ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY on a
// position with more than 1 tile in the bag, rather than silently solving
// a wrong subproblem.
static void test_bai_peg_rejects_non_one_in_bag(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/7CAT5/15/15/15/15/15/15/15 "
              "ABCDEFG/HIJKLMN 0/0 0 -lex NWL20");
  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) > 1);

  BaiPegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .num_threads = 1,
      .tt_fraction_of_mem = 0.0,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .max_depth = 1,
      .endgame_time_per_solve = 0.1,
      .time_budget_seconds = 1.0,
      .puct_c = 1.0,
  };
  BaiPegResult result;
  ErrorStack *error_stack = error_stack_create();
  bai_peg_solve(&args, &result, error_stack);
  assert(!error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_bai_peg(void) {
  test_bai_peg_smoke();
  test_bai_peg_rejects_non_one_in_bag();
}
