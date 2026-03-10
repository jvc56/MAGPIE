#include "peg_test.h"

#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "../src/util/string_util.h"
#include "peg_test_util.h"
#include "test_util.h"

#include <assert.h>
#include <string.h>

// From macondo TestStraightforward1PEG (NWL20).
// Mover has ENOSTXY, 1 tile in bag (8 unseen total: 1 bag + 7 opponent tiles).
// Greedy (pass 0) finds (WE)EN at 75% win / 6 out of 8 scenarios; ONYX ranks
// around #7 at 62.5%.  Pass is evaluated recursively (opp has ACEISUY and
// scores well, so pass drops below the top plays).  4 endgame passes refine
// the top-K candidates with progressively deeper search (default limits:
// 64→32→16→8 candidates per pass) using a shared ~4 GB TT (tt=0.5 of mem).
// The key assertion is that the best move has positive expected spread.
static void test_peg_straightforward(void) {
  Config *config =
      config_create_or_die("set -wmp false -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(
      config,
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/"
      "E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/"
      "1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex "
      "NWL20");

  Game *game = config_get_game(config);
  assert(bag_get_letters(game_get_bag(game)) == 1);
  peg_test_print_game_position(game);

  PegSolver *solver = peg_solver_create();
  PegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .time_budget_seconds = 0.0,
      .num_threads = 8,
      .tt_fraction_of_mem = 0.5, // ~4 GB TT, shared within each pass
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .num_stages = 3,
      .stage_candidate_limits = {24, 10},
      .early_cutoff = true,
      .per_pass_callback = peg_test_progress_callback,
      .first_win_mode = PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,
  };

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  peg_solve(solver, &args, &result, error_stack);

  assert(error_stack_is_empty(error_stack));
  assert(result.stages_completed == 3);
  // Best move should be 13L ONYX with win% = 7.5/8 = 93.75%.
  assert(move_get_type(&result.best_move) != GAME_EVENT_PASS);
  {
    Move best = result.best_move;
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), &best,
                            game_get_ld(game), false);
    assert(strcmp(string_builder_peek(sb), "13L ONYX") == 0);
    string_builder_destroy(sb);
  }
  assert(result.best_win_pct > 0.93 && result.best_win_pct < 0.94);
  peg_test_print_result(&result, game);

  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// From macondo Test1PEGPass (FRA20, French).
// The macondo CGP omits the opponent's rack (empty string after '/'); MAGPIE's
// peg_solve handles this correctly because it computes unseen tiles from the
// full distribution minus the mover's rack and board tiles, ignoring whatever
// the game struct holds for the opponent.  The macondo CGP also has a short
// row 3 ("8A1E1DO", 14 squares); the missing trailing blank is restored.
// macondo's Test1PEGPass asserts that pass wins using a 102-tile French
// distribution (8 unseen tiles).  MAGPIE's french.csv also has 102 tiles,
// giving 8 unseen tiles — the same setup.  The pass candidate is force-
// included in the 1-ply refinement even though it ranks far below the top
// greedy plays (TSARINE/RESINAT score +63, 100% greedy win rate).
// This test uses pass_candidate_limits={2} (top 2 plays + pass = 3 total
// in 1-ply endgame) for speed.  Pass is evaluated recursively: for each
// bag-tile scenario T, peg_solve is called from opp's perspective (opp rack
// = unseen-T, bag has T, skip_pass=1 to prevent infinite recursion); the
// opp's best play is compared with the "opp also passes" outcome, and opp
// picks whichever is better for them.
static void test_peg_endgame_pass(void) {
  Config *config =
      config_create_or_die("set -wmp false -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(
      config,
      "cgp 11ONZE/10J2O1/8A1E1DO1/7QUETEE1H/10E1F1U/8ECUMERA/8C1R1TIR/"
      "7WOKS2ET/6DUR6/5G2N1M4/4HALLALiS3/1G1P1P1OM1XI3/VIVONS1BETEL3/"
      "IF1N3AS1RYAL1/ETUDIAIS7 AEINRST/ 301/300 0 -lex FRA20 -ld french");

  Game *game = config_get_game(config);
  peg_test_print_game_position(game);

  PegSolver *solver = peg_solver_create();
  PegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .time_budget_seconds = 0.0,
      .num_threads = 8,
      .tt_fraction_of_mem = 0.5,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .num_stages = 2,
      .stage_candidate_limits = {7},
      .early_cutoff = true,
      .per_pass_callback = peg_test_progress_callback,
      .first_win_mode = PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,
  };

  PegResult result;
  ErrorStack *error_stack = error_stack_create();
  peg_solve(solver, &args, &result, error_stack);

  assert(error_stack_is_empty(error_stack));
  assert(result.stages_completed == 2);
  assert(result.candidates_remaining >= 1);
  peg_test_print_result(&result, game);

  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Verify that peg_solve rejects a fully-played-out position with 0 unseen tiles.
static void test_peg_no_unseen_error(void) {
  Config *config =
      config_create_or_die("set -wmp false -s1 score -s2 score -r1 small -r2 small");
  // Both racks empty, bag empty, board fully covered by score = total tiles.
  // Use an endgame position where the bag is empty and both racks are empty.
  load_and_exec_config_or_die(
      config,
      "cgp 9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/"
      "8WE2OBI/6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/"
      "FIVE1E5IT1C/5SPORRAN2A/6ORE2N2D / 384/389 0 -lex NWL20");

  Game *game = config_get_game(config);
  peg_test_print_game_position(game);

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

void test_peg(void) {
  test_peg_straightforward();
  test_peg_endgame_pass();
  test_peg_no_unseen_error();
}
