#include <assert.h>
#include <stddef.h>

#include "../src/config.h"
#include "../src/log.h"

#include "config_test.h"

void test_config_error(Config *config, const char *cmd,
                       config_load_status_t expected_status) {
  config_load_status_t actual_status = load_config(config, cmd);
  if (actual_status != expected_status) {
    printf("config status mismatched: %d != %d\n", expected_status,
           actual_status);
    assert(0);
  }
}

void test_config_error_cases() {
  Config *config = create_default_config();
  test_config_error(config, "position gcg",
                    CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG);
  test_config_error(config, "go sim lex CSW21 i 1000 plies 10 1",
                    CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG);
  test_config_error(config, "go sim sim", CONFIG_LOAD_STATUS_DUPLICATE_ARG);
  test_config_error(config, "sim go", CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND);
  test_config_error(config, "go sim i 1000 infer",
                    CONFIG_LOAD_STATUS_MISPLACED_COMMAND);
  test_config_error(config, "go sim i 1000",
                    CONFIG_LOAD_STATUS_LEXICON_MISSING);
  test_config_error(config, "go sim lex CSW21 i 1000 plies",
                    CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES);
  test_config_error(config, "position cgp 1 2 3",
                    CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES);
  test_config_error(config, "go sim bdn Scrimbool",
                    CONFIG_LOAD_STATUS_UNKNOWN_BOARD_LAYOUT);
  test_config_error(config, "go sim var Lonify",
                    CONFIG_LOAD_STATUS_UNKNOWN_GAME_VARIANT);
  test_config_error(config, "go sim bb 3b4",
                    CONFIG_LOAD_STATUS_MALFORMED_BINGO_BONUS);
  test_config_error(config, "go sim s1 random",
                    CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE);
  test_config_error(config, "go sim s2 none",
                    CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE);
  test_config_error(config, "go sim r1 top",
                    CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE);
  test_config_error(config, "go sim r2 3",
                    CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE);
  test_config_error(config, "go sim lex CSW21 rack ABCD1EF",
                    CONFIG_LOAD_STATUS_MALFORMED_RACK);
  test_config_error(config, "go sim lex CSW21 rack 6",
                    CONFIG_LOAD_STATUS_MALFORMED_RACK);
  test_config_error(config, "go sim numplays three",
                    CONFIG_LOAD_STATUS_MALFORMED_NUM_PLAYS);
  test_config_error(config, "go sim numplays 123R456",
                    CONFIG_LOAD_STATUS_MALFORMED_NUM_PLAYS);
  test_config_error(config, "go sim numplays -2",
                    CONFIG_LOAD_STATUS_MALFORMED_NUM_PLAYS);
  test_config_error(config, "go sim plies two",
                    CONFIG_LOAD_STATUS_MALFORMED_PLIES);
  test_config_error(config, "go sim plies -3",
                    CONFIG_LOAD_STATUS_MALFORMED_PLIES);
  test_config_error(config, "go sim i six",
                    CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS);
  test_config_error(config, "go sim i -6",
                    CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS);
  test_config_error(config, "go sim stop 96",
                    CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION);
  test_config_error(config, "go sim stop -95",
                    CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION);
  test_config_error(config, "go sim stop NO",
                    CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION);
  test_config_error(config, "go sim pindex 3",
                    CONFIG_LOAD_STATUS_MALFORMED_PLAYER_TO_INFER_INDEX);
  test_config_error(config, "go sim pindex -1",
                    CONFIG_LOAD_STATUS_MALFORMED_PLAYER_TO_INFER_INDEX);
  test_config_error(config, "go sim pindex one",
                    CONFIG_LOAD_STATUS_MALFORMED_PLAYER_TO_INFER_INDEX);
  test_config_error(config, "go sim score over9000",
                    CONFIG_LOAD_STATUS_MALFORMED_SCORE);
  test_config_error(config, "go sim score -11",
                    CONFIG_LOAD_STATUS_MALFORMED_SCORE);
  test_config_error(config, "go sim eq 23434.32433.4324",
                    CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN);
  test_config_error(config, "go sim eq -3",
                    CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN);
  test_config_error(config, "go sim eq -4.5",
                    CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN);
  test_config_error(config, "go sim eq none",
                    CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN);
  test_config_error(config, "go sim exch five",
                    CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_TILES_EXCHANGED);
  test_config_error(config, "go sim exch -4",
                    CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_TILES_EXCHANGED);
  test_config_error(config, "go sim rs zero",
                    CONFIG_LOAD_STATUS_MALFORMED_RANDOM_SEED);
  test_config_error(config, "go sim rs -4",
                    CONFIG_LOAD_STATUS_MALFORMED_RANDOM_SEED);
  test_config_error(config, "go sim threads many",
                    CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS);
  test_config_error(config, "go sim threads -100",
                    CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS);
  test_config_error(config, "go sim info x",
                    CONFIG_LOAD_STATUS_MALFORMED_PRINT_INFO_INTERVAL);
  test_config_error(config, "go sim info -40",
                    CONFIG_LOAD_STATUS_MALFORMED_PRINT_INFO_INTERVAL);
  test_config_error(config, "go sim check z",
                    CONFIG_LOAD_STATUS_MALFORMED_CHECK_STOP_INTERVAL);
  test_config_error(config, "go sim check -90",
                    CONFIG_LOAD_STATUS_MALFORMED_CHECK_STOP_INTERVAL);
  test_config_error(config, "go sim l1 CSW21 l2 DISC2",
                    CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_error(config, "go sim l1 OSPS44 l2 DISC2",
                    CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  test_config_error(config, "go sim l1 NWL20 l2 OSPS44",
                    CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS);
  destroy_config(config);
}

void test_config() { test_config_error_cases(); }