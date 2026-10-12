#include "movegen_budget_test.h"

#include "../src/ent/bai_result.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

// A player's movegen budget for a move (-movebudget, -mb1, -mb2) stops its
// search when the move's generations reach it: never short of it, and past it
// by at most what its threads had in hand when it was spent.

// The status a simulation finished on.
static bai_result_status_t sim_status(Config *config) {
  return bai_result_get_status(
      sim_results_get_bai_result(config_get_sim_results(config)));
}

// A simulation with an iteration budget it could not reach stops at its
// movegen budget instead, and still ranks a best play.
static void test_a_simulation_stops_at_its_budget(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -wmp true -s1 score -s2 score -r1 all -r2 all "
      "-plies 2 -threads 2 -it 1000000000 -scond none -movebudget 20000");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack ACEIRST");
  load_and_exec_config_or_die(config, "gen");
  const uint64_t before = gen_get_movegen_count();
  load_and_exec_config_or_die(config, "sim");
  const uint64_t spent = gen_get_movegen_count() - before;
  printf("a simulation with a budget of 20000 made %" PRIu64 " generations\n",
         spent);
  assert(sim_status(config) == BAI_RESULT_STATUS_MOVEGEN_BUDGET);
  assert(spent >= 20000);
  assert(spent < 40000);
  assert(bai_result_get_best_arm(
             sim_results_get_bai_result(config_get_sim_results(config))) >= 0);
  config_destroy(config);
}

// A budget of 0 is none: the simulation ends on its iteration budget.
static void test_a_budget_of_0_is_none(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -wmp true -s1 score -s2 score -r1 all -r2 all "
      "-plies 2 -threads 2 -it 300 -scond none -movebudget 0");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  load_and_exec_config_or_die(config, "rack ACEIRST");
  load_and_exec_config_or_die(config, "gen");
  load_and_exec_config_or_die(config, "sim");
  assert(sim_status(config) == BAI_RESULT_STATUS_SAMPLE_LIMIT);
  config_destroy(config);
}

// In autoplay the budget is the player's, a move at a time: a simmer whose
// iteration budget alone would take some 300,000 generations a move plays
// two games within a few thousand a move, and its opponent is untouched.
static void test_autoplay_spends_each_move_within_its_budget(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 best "
      "-numplays 1 -gp false -threads 2 -pl1 2 -np1 10 -i1 100000 "
      "-sc1 none -si1 false -mb1 2000");
  assert(config_get_player_movegen_budget(config, 0) == 2000);
  assert(config_get_player_movegen_budget(config, 1) == 0);
  const uint64_t before = gen_get_movegen_count();
  load_and_exec_config_or_die(config, "autoplay games 2 -seed 3");
  const uint64_t spent = gen_get_movegen_count() - before;
  printf("two games with a budget of 2000 a move made %" PRIu64
         " generations\n",
         spent);
  // Some 30 moves a game for the simmer, each within its budget and a
  // little: far under what one move of its iteration budget would take.
  assert(spent < 300000);
  config_destroy(config);
}

void test_movegen_budget(void) {
  test_a_simulation_stops_at_its_budget();
  test_a_budget_of_0_is_none();
  test_autoplay_spends_each_move_within_its_budget();
}
