#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/game_history.h"
#include "../src/gcg.h"

#define GCG_DIRECTORY_FILEPATH "testdata/"

char *get_gcg_filepath(const char *filename) {
  // Calculate the lengths of the input strings
  size_t directory_path_len = strlen(GCG_DIRECTORY_FILEPATH);
  size_t str_len = strlen(filename);

  // Allocate memory for the result string
  char *result =
      (char *)malloc((directory_path_len + str_len + 1) * sizeof(char));

  if (result == NULL) {
    return NULL; // Memory allocation failed
  }

  strcpy(result, GCG_DIRECTORY_FILEPATH);

  // Concatenate the original string
  strcat(result, filename);

  return result;
}

gcg_parse_status_t test_parse_gcg(const char *gcg_filename,
                                  GameHistory *game_history) {
  char *gcg_filepath = get_gcg_filepath(gcg_filename);
  gcg_parse_status_t gcg_parse_status = parse_gcg(gcg_filepath, game_history);
  free(gcg_filepath);
  return gcg_parse_status;
}

void test_single_error_case(const char *gcg_filename,
                            gcg_parse_status_t expected_gcg_parse_status) {
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  destroy_game_history(game_history);
  printf("actual: %d\n", gcg_parse_status);
  assert(gcg_parse_status == expected_gcg_parse_status);
}

void test_error_cases() {
  test_single_error_case("duplicate_nicknames.gcg",
                         GCG_PARSE_STATUS_DUPLICATE_NAMES);
}

void test_gcg() { test_error_cases(); }
