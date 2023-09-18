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
  printf("%d == %d\n", gcg_parse_status, expected_gcg_parse_status);
  assert(gcg_parse_status == expected_gcg_parse_status);
}

void test_error_cases() {
  test_single_error_case("empty.gcg", GCG_PARSE_STATUS_GCG_EMPTY);
  test_single_error_case("unsupported_character_encoding.gcg",
                         GCG_PARSE_STATUS_UNSUPPORTED_CHARACTER_ENCODING);
  test_single_error_case("duplicate_nicknames.gcg",
                         GCG_PARSE_STATUS_DUPLICATE_NICKNAMES);
  test_single_error_case("duplicate_names.gcg",
                         GCG_PARSE_STATUS_DUPLICATE_NAMES);
  test_single_error_case("description_after_events.gcg",
                         GCG_PARSE_STATUS_PRAGMA_SUCCEEDED_EVENT);
  test_single_error_case("move_before_player.gcg",
                         GCG_PARSE_STATUS_MOVE_BEFORE_PLAYER);
  test_single_error_case("player_number_redundant.gcg",
                         GCG_PARSE_STATUS_PLAYER_NUMBER_REDUNDANT);
  test_single_error_case("encoding_wrong_place.gcg",
                         GCG_PARSE_STATUS_ENCODING_WRONG_PLACE);
  test_single_error_case("player_does_not_exist.gcg",
                         GCG_PARSE_STATUS_PLAYER_DOES_NOT_EXIST);
  test_single_error_case("note_before_events.gcg",
                         GCG_PARSE_STATUS_NOTE_PRECEDENT_EVENT);
  test_single_error_case("phony_tiles_returned_with_no_play.gcg",
                         GCG_PARSE_STATUS_PHONY_TILES_RETURNED_WITHOUT_PLAY);
  test_single_error_case("no_matching_token.gcg",
                         GCG_PARSE_STATUS_NO_MATCHING_TOKEN);
  test_single_error_case("line_overflow.gcg", GCG_PARSE_STATUS_LINE_OVERFLOW);
  test_single_error_case("lowercase_tile_placement.gcg",
                         GCG_PARSE_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  test_single_error_case("invalid_tile_placement.gcg",
                         GCG_PARSE_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  test_single_error_case("malformed_rack.gcg", GCG_PARSE_STATUS_RACK_MALFORMED);
  test_single_error_case("six_pass_last_rack_malformed.gcg",
                         GCG_PARSE_STATUS_PLAYED_LETTERS_NOT_IN_RACK);
  test_single_error_case("exchange_malformed.gcg",
                         GCG_PARSE_STATUS_PLAYED_LETTERS_NOT_IN_RACK);
}

void test_gcg() { test_error_cases(); }
