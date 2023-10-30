#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/config.h"
#include "../src/log.h"
#include "../src/string_util.h"

#include "alphabet_test.h"
#include "autoplay_test.h"
#include "bag_test.h"
#include "board_test.h"
#include "command_test.h"
#include "config_test.h"
#include "cross_set_test.h"
#include "equity_adjustment_test.h"
#include "game_test.h"
#include "gameplay_test.h"
#include "gcg_test.h"
#include "infer_test.h"
#include "leave_map_test.h"
#include "leaves_test.h"
#include "letter_distribution_test.h"
#include "movegen_test.h"
#include "players_data_test.h"
#include "rack_test.h"
#include "shadow_test.h"
#include "sim_test.h"
#include "stats_test.h"
#include "string_util_test.h"
#include "test_constants.h"
#include "testconfig.h"
#include "wasm_api_test.h"
#include "word_test.h"

void run_all(TestConfig *testconfig) {
  // Test the loading of the config
  test_players_data();
  test_config();

  // Test the readonly data first
  test_string_util();
  test_alphabet(testconfig);
  test_letter_distribution(testconfig);
  test_str_to_machine_letters(testconfig);
  test_leaves(testconfig);
  test_leave_map(testconfig);

  // Now test the rest
  test_bag(testconfig);
  test_rack(testconfig);
  test_board(testconfig);
  test_cross_set(testconfig);
  test_game(testconfig);
  test_shadow(testconfig);
  test_movegen(testconfig);
  test_equity_adjustments(testconfig);
  test_gameplay(testconfig);
  test_stats();
  test_infer(testconfig);
  test_sim(testconfig);
  test_command(testconfig);
  test_gcg();
  test_autoplay(testconfig);
  test_wasm_api();
}

void run_test(TestConfig *testconfig, const char *subtest) {
  if (strings_equal(subtest, "config")) {
    test_config();
  } else if (strings_equal(subtest, "players")) {
    test_players_data();
  } else if (strings_equal(subtest, "stringutil")) {
    test_string_util();
  } else if (strings_equal(subtest, "alphabet")) {
    test_alphabet(testconfig);
  } else if (strings_equal(subtest, "letterdistribution")) {
    test_letter_distribution(testconfig);
  } else if (strings_equal(subtest, "strtomachineletters")) {
    test_str_to_machine_letters(testconfig);
  } else if (strings_equal(subtest, "leaves")) {
    test_leaves(testconfig);
  } else if (strings_equal(subtest, "leavemap")) {
    test_leave_map(testconfig);
  } else if (strings_equal(subtest, "bag")) {
    test_bag(testconfig);
  } else if (strings_equal(subtest, "rack")) {
    test_rack(testconfig);
  } else if (strings_equal(subtest, "board")) {
    test_board(testconfig);
  } else if (strings_equal(subtest, "crossset")) {
    test_cross_set(testconfig);
  } else if (strings_equal(subtest, "game")) {
    test_game(testconfig);
  } else if (strings_equal(subtest, "shadow")) {
    test_shadow(testconfig);
  } else if (strings_equal(subtest, "movegen")) {
    test_movegen(testconfig);
  } else if (strings_equal(subtest, "equityadjustments")) {
    test_equity_adjustments(testconfig);
  } else if (strings_equal(subtest, "gameplay")) {
    test_gameplay(testconfig);
  } else if (strings_equal(subtest, "stats")) {
    test_stats(testconfig);
  } else if (strings_equal(subtest, "infer")) {
    test_infer(testconfig);
  } else if (strings_equal(subtest, "sim")) {
    test_sim(testconfig);
  } else if (strings_equal(subtest, "command")) {
    test_command(testconfig);
  } else if (strings_equal(subtest, "gcg")) {
    test_gcg(testconfig);
  } else if (strings_equal(subtest, "autoplay")) {
    test_autoplay(testconfig);
  } else if (strings_equal(subtest, "wasm")) {
    test_wasm_api(testconfig);
  } else {
    log_warn("skipping unrecognized test: %s\n", subtest);
  }
}

int main(int argc, char *argv[]) {
  log_set_level(LOG_WARN);

  TestConfig *testconfig = create_testconfig(
      // CSW
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1000000",
      // NWL
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1000000",
      // OSPS
      "setoptions lex OSPS44 s1 equity s2 equity r1 all r2 all numplays "
      "1000000",
      // DISC
      "setoptions lex DISC2 s1 equity s2 equity r1 all r2 all numplays 1000000",
      // Distinct lexica
      "setoptions l1 CSW21 l2 NWL20 s1 equity s2 equity r1 all r2 all numplays "
      "1000000");

  if (argc == 1) {
    run_all(testconfig);
  } else {
    for (int i = 1; i < argc; i++) {
      run_test(testconfig, argv[i]);
    }
  }
  destroy_testconfig(testconfig);
}