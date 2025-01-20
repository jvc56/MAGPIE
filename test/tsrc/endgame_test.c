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

  endgame_solver_destroy(solver);
  config_destroy(config);
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
  int plies = 8; // 8
  EndgameSolver *solver = endgame_solver_create(
      config_get_thread_control(config), config_get_game(config));
  PVLine solution = endgame_solve(solver, plies);
  assert(solution.score == -60);
  assert(small_move_is_pass(&solution.moves[0]));

  endgame_solver_destroy(solver);
  config_destroy(config);
}

void test_solve_standard(void) {
  // A standard out-in-two endgame.
  Config *config = config_create_or_die(
      "set -s1 score -s2 score -r1 small -r2 small -threads 1");
  load_and_exec_config_or_die(
      config, "cgp "
              "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
              "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
              "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20");

  int plies = 4;
  EndgameSolver *solver = endgame_solver_create(
      config_get_thread_control(config), config_get_game(config));
  PVLine solution = endgame_solve(solver, plies);
  assert(solution.score == 11);
  assert(!small_move_is_pass(&solution.moves[0]));

  endgame_solver_destroy(solver);
  config_destroy(config);
}

void test_very_deep(void) {
  Config *config = config_create_or_die(
      "set -s1 score -s2 score -r1 small -r2 small -threads 1");
  load_and_exec_config_or_die(
      config, "cgp "
              "14C/13QI/12FIE/10VEE1R/9KIT2G/8CIG1IDE/8UTA2AS/7ST1SYPh1/6JA5A1/"
              "5WOLD2BOBA/3PLOT1R1NU1EX/Y1VEIN1NOR1mOA1/UT1AT1N1L2FEH1/"
              "GUR2WIRER5/SNEEZED8 ADENOOO/AHIILMM 353/236 0 -lex CSW21;");
  // This insane endgame requires 25 plies to solve. We end up winning by 1 pt.
  int plies = 25;
  EndgameSolver *solver = endgame_solver_create(
      config_get_thread_control(config), config_get_game(config));
  PVLine solution = endgame_solve(solver, plies);
  assert(solution.score == -116);
  assert(small_move_is_pass(&solution.moves[0]));

  endgame_solver_destroy(solver);
  config_destroy(config);
}

void test_small_arena_realloc(void) {
  Config *config = config_create_or_die(
      "set -s1 score -s2 score -r1 small -r2 small -threads 1");
  load_and_exec_config_or_die(
      config, "cgp "
              "9A1PIXY/9S1L3/2ToWNLETS1O3/9U1DA1R/3GERANIAL1U1I/9g2T1C/8WE2OBI/"
              "6EMU4ON/6AID3GO1/5HUN4ET1/4ZA1T4ME1/1Q1FAKEY3JOES/FIVE1E5IT1C/"
              "5SPORRAN2A/6ORE2N2D BGIV/DEHILOR 384/389 0 -lex NWL20");

  int plies = 4;
  EndgameSolver *solver = endgame_solver_create(
      config_get_thread_control(config), config_get_game(config));
  solver->initial_small_move_arena_size = 512; // 512 bytes holds like 32 moves.
  PVLine solution = endgame_solve(solver, plies);
  assert(solution.score == 11);
  assert(!small_move_is_pass(&solution.moves[0]));

  endgame_solver_destroy(solver);
  config_destroy(config);
}

void test_endgame(void) {
  log_set_level(LOG_INFO);
//   test_solve_standard();
//   test_very_deep();
//   test_small_arena_realloc();
  // Uncomment out more of these tests once we add more optimizations,
  // and/or if we can run the endgame tests in release mode.
  // test_pass_first();
  test_vs_joey();
  log_set_level(LOG_WARN);
}