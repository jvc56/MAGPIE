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
#include "play_recorder_test.h"
#include "prof_tests.h"
#include "rack_test.h"
#include "shadow_test.h"
#include "sim_test.h"
#include "stats_test.h"
#include "string_util_test.h"
#include "test_constants.h"
#include "testconfig.h"
#include "ucgi_command_test.h"
#include "wasm_api_test.h"
#include "word_test.h"

void unit_tests(TestConfig *testconfig) {
  // Test the loading of the config
  test_config();

  // Test the readonly data first
  test_string_util();
  test_alphabet(testconfig);
  test_letter_distribution(testconfig);
  test_str_to_machine_letters(testconfig);
  test_leaves(testconfig, "./data/lexica/CSW21.csv");
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
  test_ucgi_command();
  test_gcg();
  test_autoplay(testconfig);
  test_wasm_api();
}

void run_tests(TestConfig *testconfig, const char *subtest) {
  log_fatal("unrecognized test: %s\n", subtest);
}

int main(int argc, char *argv[]) {
  Config *csw_config = create_config(
      "./data/letterdistributions/english.csv", "", "./data/lexica/CSW21.kwg",
      "./data/lexica/CSW21.klv2", MOVE_SORT_EQUITY, MOVE_RECORDER_ALL, "", "",
      -1, -1, 0, 10000, 0, 0, NULL, 0, 0, 0, 0, 1,
      "./data/strategy/default_english/winpct.csv", TEST_MOVE_LIST_CAPACITY);

  Config *nwl_config = create_config(
      "./data/letterdistributions/english.csv", "", "./data/lexica/NWL20.kwg",
      "./data/lexica/CSW21.klv2", MOVE_SORT_SCORE, MOVE_RECORDER_ALL, "", "",
      -1, -1, 0, 10000, 0, 0, NULL, 0, 0, 0, 0, 1,
      "./data/strategy/default_english/winpct.csv", TEST_MOVE_LIST_CAPACITY);

  Config *osps_config = create_config(
      // no OSPS kwg yet, use later when we have tests.
      "./data/letterdistributions/polish.csv", "", "./data/lexica/OSPS44.kwg",
      "", MOVE_SORT_EQUITY, MOVE_RECORDER_ALL, "", "", -1, -1, 0, 10000, 0, 0,
      NULL, 0, 0, 0, 0, 1, "./data/strategy/default_english/winpct.csv",
      TEST_MOVE_LIST_CAPACITY);

  Config *disc_config = create_config(
      "./data/letterdistributions/catalan.csv", "", "./data/lexica/DISC2.kwg",
      "./data/lexica/catalan.klv2", MOVE_SORT_EQUITY, MOVE_RECORDER_ALL, "", "",
      -1, -1, 0, 10000, 0, 0, NULL, 0, 0, 0, 0, 1,
      "./data/strategy/default_english/winpct.csv", TEST_MOVE_LIST_CAPACITY);

  Config *distinct_lexica_config = create_config(
      "./data/letterdistributions/english.csv", "", "./data/lexica/CSW21.kwg",
      "./data/lexica/CSW21.klv2", MOVE_SORT_EQUITY, MOVE_RECORDER_ALL,
      "./data/lexica/NWL20.kwg", "", -1, -1, 0, 10000, 0, 0, NULL, 0, 0, 0, 0,
      1, "./data/strategy/default_english/winpct.csv", TEST_MOVE_LIST_CAPACITY);

  TestConfig *testconfig = create_testconfig(
      // CSW
      "set options lex CSW21",

      nwl_config, osps_config, disc_config, distinct_lexica_config);

  const char *subtest = NULL;
  if (argc > 1) {
    subtest = argv[1];
  }
  log_set_level(LOG_WARN);
  run_tests(test_config, subtest)
}