#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/config.h"

#include "alphabet_test.h"
#include "bag_test.h"
#include "board_test.h"
#include "config_test.h"
#include "cross_set_test.h"
#include "equity_adjustment_test.h"
#include "game_test.h"
#include "gen_all_test.h"
#include "gameplay_test.h"
#include "leaves_test.h"
#include "movegen_test.h"
#include "prof_tests.h"
#include "rack_test.h"
#include "test_constants.h"
#include "superconfig.h"

void unit_tests(SuperConfig * superconfig) {
    // Test the loading of the config
    test_config();

    // Test the readonly data first
    test_alphabet(superconfig);
    test_leaves(superconfig);

    // Now test the rest
    test_bag(superconfig);
    test_rack(superconfig);
    test_board(superconfig);
    test_cross_set(superconfig);
    test_game(superconfig);
    test_movegen(superconfig);
    test_equity_adjustments(superconfig);
    test_gameplay(superconfig);
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
      "./data/lexica/CSW21.kwg",
      "./data/lexica/CSW21_zeroblank.alph",
      "./data/letterdistributions/english_zeroblank.dist",
      "",
      "./data/lexica/CSW21_zeroblank.laddag",
      SORT_BY_EQUITY,
      PLAY_RECORDER_TYPE_ALL,
      "",
      -1,
      -1,
      0,
      10000
    );
    end = clock();
    printf("loading csw config took %0.6f seconds\n", (double)(end - begin) / CLOCKS_PER_SEC);
  
    begin = clock();
    Config * america_config = create_config(
      "./data/lexica/America.kwg",
      "./data/lexica/CSW21_zeroblank.alph",
      "./data/letterdistributions/english_zeroblank.dist",
      "",
      "./data/lexica/CSW21_zeroblank.laddag",
      SORT_BY_SCORE,
      PLAY_RECORDER_TYPE_ALL,
      "",
      -1,
      -1,
      0,
      10000
    );
    end = clock();
    printf("loading america config took %0.6f seconds\n", (double)(end - begin) / CLOCKS_PER_SEC);
    
    begin = clock();
    SuperConfig * superconfig = create_superconfig(csw_config, america_config);
    unit_tests(superconfig);
    end = clock();
    printf("unit tests took %0.6f seconds\n", (double)(end - begin) / CLOCKS_PER_SEC);

    // This also frees the nested configs
    destroy_superconfig(superconfig);
  } else {
    printf("unrecognized command: %s\n", argv[1]);
    exit(EXIT_FAILURE);
  }
}