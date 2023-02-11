#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/config.h"

#include "alphabet_test.h"
#include "bag_test.h"
#include "board_test.h"
#include "cross_set_test.h"
#include "equity_adjustment_test.h"
#include "game_test.h"
#include "gameplay_test.h"
#include "leaves_test.h"
#include "letter_distribution_test.h"
#include "movegen_test.h"
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
  Config * config = create_config_from_args(argc, argv);

  if (!strcmp(config->command, CMD_GEN)) {
    // run the print gen all
    printf("unimplemented\n");
    abort();
  } else if (!strcmp(config->command, CMD_UNIT_TESTS)) {
    // Assumes the unit test is called from the 'core' directory
    TestConfig * test_config = create_test_config(create_config(
      "./data/lexica/CSW21.gaddag",
      "./data/lexica/CSW21.alph",
      "./data/letterdistributions/english.dist",
      "./data/lexica/CSW21.laddag",
      SORT_BY_EQUITY,
      PLAY_RECORDER_TYPE_ALL,
      CMD_UNIT_TESTS
    ), create_config(
      "./data/lexica/America.gaddag",
      "./data/lexica/CSW21.alph",
      "./data/letterdistributions/english.dist",
      "./data/lexica/CSW21.laddag",
      SORT_BY_SCORE,
      PLAY_RECORDER_TYPE_ALL,
      CMD_UNIT_TESTS
    )
    );
    unit_tests(test_config);
    // This also frees the nested configs
    destroy_test_config(test_config);
  } else {
    printf("command not recognized: %s\n", config->command);
    exit(EXIT_FAILURE);
  }

  
}