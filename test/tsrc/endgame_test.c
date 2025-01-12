#include <assert.h>

#include "../../src/impl/config.h"
#include "../../src/impl/endgame.h"
#include "../../src/util/log.h"
#include "test_util.h"

void test_vs_joey(void) {
  Config *config =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small "
                           "-threads 1");
  load_and_exec_config_or_die(
      config,
      "cgp "
      "AIDER2U7/b1E1E2N1Z5/AWN1T2M1ATT3/LI1COBLE2OW3/OP2U2E2AA3/NE2CUSTARDS1Q1/"
      "ER1OH5I2U1/S2K2FOB1ERGOT/5HEXYLS2I1/4JIN6N1/2GOOP2NAIVEsT/1DIRE10/"
      "2GAY10/15/15 AEFILMR/DIV 371/412 0 -lex NWL20;");

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
  EndgameSolver *solver = endgame_solver_create(
      config_get_thread_control(config), config_get_game(config));

  Player *p1 = game_get_player(config_get_game(config), 0);
  Rack *r1 = player_get_rack(p1);
  Player *p2 = game_get_player(config_get_game(config), 1);
  Rack *r2 = player_get_rack(p2);

  log_warn("Rack1: %d", rack_get_total_letters(r1));
  log_warn("Rack2: %d", rack_get_total_letters(r2));

  PVLine solution = endgame_solve(solver, 13);
  assert(solution.score == 55);
}

void test_pass_first(void) {
  Config *config = config_create_or_die(
      "set -s1 score -s2 score -r1 small -r2 small -threads 1");
  load_and_exec_config_or_die(
      config,
      "cgp "
      "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/"
      "1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/"
      "3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW21");
  // This endgame's first move must be a pass, otherwise Nigel can set up
  // an unblockable ZA.
  int plies = 8;
  EndgameSolver *solver = endgame_solver_create(
      config_get_thread_control(config), config_get_game(config));
  PVLine solution = endgame_solve(solver, plies);
  assert(solution.score == -60);
  assert(solution.moves[0].move_type == GAME_EVENT_PASS);
}

void test_endgame(void) { test_pass_first(); }