#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/config.h"

#include "alphabet_test.h"
#include "bag_test.h"
#include "board_test.h"
#include "cross_set_test.h"
#include "equity_adjustment_test.h"
#include "game_test.h"
#include "gen_all_test.h"
#include "gameplay_test.h"
#include "leaves_test.h"
#include "letter_distribution_test.h"
#include "movegen_test.h"
#include "prof_tests.h"
#include "rack_test.h"
#include "test_constants.h"
#include "test_config.h"

void unit_tests(TestConfig * test_config) {
    // Test the readonly data first
    test_alphabet(test_config);
    test_leaves(test_config);
    test_letter_distribution(test_config);

    // Now test the rest
    test_bag(test_config);
    test_rack(test_config);
    test_board(test_config);
    test_cross_set(test_config);
    test_game(test_config);
    test_movegen(test_config);
    test_equity_adjustments(test_config);
    test_gameplay(test_config);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("must specify exactly one command\n");
    exit(EXIT_FAILURE);
  }
  
  if (!strcmp(argv[1], CMD_GEN)) {
    Config * config = create_config_from_args(argc, argv);
    test_gen_all(config);
    destroy_config(config);
  } else if (!strcmp(argv[1], CMD_PROF)) { 
    Config * config = create_config_from_args(argc, argv);
    prof_tests(config);
    destroy_config(config);
  } else if (!strcmp(argv[1], CMD_UNIT_TESTS)) {
    // Assumes the unit test is called from the 'core' directory
    clock_t begin;
    clock_t end;

    begin = clock();
    Config * csw_config = create_config(
      "./data/lexica/CSW21.gaddag",
      "./data/lexica/CSW21.alph",
      "./data/letterdistributions/english.dist",
      "",
      "./data/lexica/CSW21.laddag",
      SORT_BY_EQUITY,
      PLAY_RECORDER_TYPE_ALL,
      "",
      -1,
      -1
    );
    end = clock();
    printf("loading csw config took %0.6f seconds\n", (double)(end - begin) / CLOCKS_PER_SEC);
  
    begin = clock();
    Config * america_config = create_config(
      "./data/lexica/America.gaddag",
      "./data/lexica/CSW21.alph",
      "./data/letterdistributions/english.dist",
      "",
      "./data/lexica/CSW21.laddag",
      SORT_BY_SCORE,
      PLAY_RECORDER_TYPE_ALL,
      "",
      -1,
      -1
    );
    end = clock();
    printf("loading america config took %0.6f seconds\n", (double)(end - begin) / CLOCKS_PER_SEC);
    
    begin = clock();
    TestConfig * test_config = create_test_config(csw_config, america_config);
    unit_tests(test_config);
    end = clock();
    printf("unit tests took %0.6f seconds\n", (double)(end - begin) / CLOCKS_PER_SEC);

    // This also frees the nested configs
    destroy_test_config(test_config);
  } else {
    printf("unrecognized command: %s\n", argv[1]);
    exit(EXIT_FAILURE);
  }
}