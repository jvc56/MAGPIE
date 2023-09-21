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
  test_single_error_case("exchange_not_in_rack.gcg",
                         GCG_PARSE_STATUS_PLAYED_LETTERS_NOT_IN_RACK);
  test_single_error_case("exchange_malformed.gcg",
                         GCG_PARSE_STATUS_PLAY_MALFORMED);
  test_single_error_case("malformed_play.gcg", GCG_PARSE_STATUS_PLAY_MALFORMED);
  test_single_error_case("played_tile_not_in_rack.gcg",
                         GCG_PARSE_STATUS_PLAYED_LETTERS_NOT_IN_RACK);
  test_single_error_case("play_out_of_bounds.gcg",
                         GCG_PARSE_STATUS_PLAY_OUT_OF_BOUNDS);
  test_single_error_case("redundant_pragma.gcg",
                         GCG_PARSE_STATUS_REDUNDANT_PRAGMA);
  test_single_error_case("game_events_overflow.gcg",
                         GCG_PARSE_STATUS_GAME_EVENTS_OVERFLOW);
}

void test_parse_special_char() {
  const char *gcg_filename = "name_iso8859-1.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert(!strcmp(game_history->players[0]->name, "césar"));
  assert(!strcmp(game_history->players[1]->name, "hércules"));
  destroy_game_history(game_history);
}

void test_parse_special_utf8_no_header() {
  const char *gcg_filename = "name_utf8_noheader.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert(!strcmp(game_history->players[0]->name, "cÃ©sar"));
  destroy_game_history(game_history);
}

void test_parse_special_utf8_with_header() {
  const char *gcg_filename = "name_utf8_with_header.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert(!strcmp(game_history->players[0]->name, "césar"));
  destroy_game_history(game_history);
}

void test_parse_dos_mode() {
  const char *gcg_filename = "utf8_dos.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert(!strcmp(game_history->players[0]->nickname, "angwantibo"));
  assert(!strcmp(game_history->players[1]->nickname, "Michal_Josko"));
  destroy_game_history(game_history);
}

void test_success_standard() {
  const char *gcg_filename = "success_standard.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert(!strcmp(game_history->title, "test game"));
  assert(!strcmp(game_history->description, "Created with Macondo"));
  assert(!strcmp(game_history->id_auth, "io.woogles"));
  assert(!strcmp(game_history->uid, "p6QRjJHG"));
  assert(!strcmp(game_history->lexicon_name, "CSW21"));
  assert(!strcmp(game_history->letter_distribution_name, "english"));
  assert(game_history->game_variant == GAME_VARIANT_CLASSIC);
  assert(game_history->board_layout == BOARD_LAYOUT_CROSSWORD_GAME);
  assert(!strcmp(game_history->players[0]->nickname, "HastyBot"));
  assert(game_history->players[0]->score == 516);
  assert(game_history->players[0]->last_known_rack == NULL);
  assert(!strcmp(game_history->players[1]->nickname, "RightBehindYou"));
  assert(game_history->players[1]->score == 358);
  assert(game_history->players[1]->last_known_rack == NULL);
  assert(game_history->number_of_events == 29);
  destroy_game_history(game_history);
}

void test_gcg() {
  test_error_cases();
  test_parse_special_char();
  test_parse_special_utf8_no_header();
  test_parse_special_utf8_with_header();
  test_parse_dos_mode();
  test_success_standard();
  // test multiline note
}
