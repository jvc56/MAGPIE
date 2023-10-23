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

void test_config() {
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
  destroy_config(config);
}