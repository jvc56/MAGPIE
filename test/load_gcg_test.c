#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

// Helper function to validate GCG download
void validate_load_gcg(const char *source_identifier,
                       error_code_t expected_error) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  printf("Testing with identifier: %s\n", source_identifier);
  char *cmd = get_formatted_string("load %s", source_identifier);
  assert_config_exec_status(config, cmd, expected_error);
  free(cmd);
  config_destroy(config);
}

void test_load_gcg(void) {
  printf("Running load GCG tests...\n");

  printf("=== Cross-tables Tests ===\n");
  validate_load_gcg("54938", ERROR_STATUS_SUCCESS);
  validate_load_gcg("https://cross-tables.com/"
                    "annotated.php?u=5493899999999999999999999999999999999",
                    ERROR_STATUS_XT_URL_MALFORMED);
  validate_load_gcg("5493899999999999999999999999999999999",
                    ERROR_STATUS_XT_ID_MALFORMED);
  // Test xt URL download and numerical ID download
  validate_load_gcg("https://cross-tables.com/annotated.php?u=54938",
                    ERROR_STATUS_SUCCESS);
  validate_load_gcg("54938", ERROR_STATUS_SUCCESS);

  printf("Cross-tables tests completed.\n\n");

  printf("=== Woogles Tests ===\n");
  // Test Woogles URL download and numerical ID download
  validate_load_gcg(
      "https://woogles.io/game/XuoAntzDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD",
      ERROR_STATUS_WOOGLES_URL_MALFORMED);
  validate_load_gcg("XuoAntzDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD",
                    ERROR_STATUS_WOOGLES_ID_MALFORMED);
  validate_load_gcg("https://woogles.io/game/XuoAntzD", ERROR_STATUS_SUCCESS);
  validate_load_gcg("XuoAntzD", ERROR_STATUS_SUCCESS);

  printf("Woogles tests completed.\n\n");

  printf("=== Local File Tests ===\n");
  // Test local file download and local file failure case
  validate_load_gcg("testdata/gcgs/success_six_pass.gcg", ERROR_STATUS_SUCCESS);
  validate_load_gcg("/tmp/nonexistent_file.gcg",
                    ERROR_STATUS_INVALID_GCG_SOURCE);
  printf("Local file tests completed.\n\n");

  printf("=== URL Download Tests ===\n");
  // Test direct URL download and failure case
  validate_load_gcg(
      "https://www.cross-tables.com/annotated/selfgcg/556/anno55690.gcg",
      ERROR_STATUS_SUCCESS);
  validate_load_gcg("https://keep.google.com/u/0/",
                    ERROR_STATUS_GCG_PARSE_GAME_EVENT_BEFORE_PLAYER);
  printf("URL download tests completed.\n\n");

  printf("=== Total Failure Test ===\n");
  // Test total failure case
  validate_load_gcg("98bfakdna\?\?}}", ERROR_STATUS_INVALID_GCG_SOURCE);

  printf("Load tests completed.\n");
}