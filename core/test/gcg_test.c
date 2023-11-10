#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/game_history.h"
#include "../src/gcg.h"
#include "../src/util.h"

#include "test_constants.h"
#include "test_util.h"

char *get_gcg_filepath(const char *filename) {
  return get_formatted_string("%s%s", TESTDATA_FILEPATH, filename);
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
  assert_strings_equal(game_history->players[0]->name, "césar");
  assert_strings_equal(game_history->players[1]->name, "hércules");
  destroy_game_history(game_history);
}

void test_parse_special_utf8_no_header() {
  const char *gcg_filename = "name_utf8_noheader.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert(strings_equal(game_history->players[0]->name, "cÃ©sar"));
  destroy_game_history(game_history);
}

void test_parse_special_utf8_with_header() {
  const char *gcg_filename = "name_utf8_with_header.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert(strings_equal(game_history->players[0]->name, "césar"));
  destroy_game_history(game_history);
}

void test_parse_dos_mode() {
  const char *gcg_filename = "utf8_dos.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert(strings_equal(game_history->players[0]->nickname, "angwantibo"));
  assert(strings_equal(game_history->players[1]->nickname, "Michal_Josko"));
  destroy_game_history(game_history);
}

void assert_game_event(GameHistory *game_history, int event_index,
                       game_event_t event_type, int player_index,
                       int cumulative_score, const char *rack_string,
                       const char *note, game_event_t move_type, int vertical,
                       int move_row_start, int move_col_start, int move_score,
                       int tiles_played, int tiles_length,
                       const char *tiles_string,
                       LetterDistribution *letter_distribution) {
  GameEvent *game_event = game_history->events[event_index];

  // Game Event assertions
  assert(game_event->event_type == event_type);
  assert(game_event->player_index == player_index);
  assert(game_event->cumulative_score == cumulative_score);
  Rack *expected_rack = NULL;
  bool racks_match = false;
  if (string_length(rack_string) > 0) {
    expected_rack = create_rack(letter_distribution->size);
    set_rack_to_string(expected_rack, rack_string, letter_distribution);
    racks_match = racks_are_equal(expected_rack, game_event->rack);
    destroy_rack(expected_rack);
  } else {
    racks_match = racks_are_equal(expected_rack, game_event->rack);
  }

  assert(racks_match);
  assert((!game_event->note && string_length(note) == 0) ||
         strings_equal(game_event->note, note));

  // Move assertions
  Move *move = game_event->move;
  if (move) {

    assert(move->move_type == move_type);
    assert(move->score == move_score);

    if (move_type != GAME_EVENT_PASS) {
      assert(move->tiles_played == tiles_played);
      assert(move->tiles_length == tiles_length);
      uint8_t *machine_letters =
          malloc_or_die(sizeof(char) * (tiles_length + 1));
      int number_of_machine_letters = str_to_machine_letters(
          game_history->letter_distribution, tiles_string,
          move_type == GAME_EVENT_TILE_PLACEMENT_MOVE, machine_letters);
      int corrected_tiles_length = tiles_length;
      if (move_type == GAME_EVENT_EXCHANGE) {
        corrected_tiles_length--;
      }
      bool tiles_match = number_of_machine_letters == corrected_tiles_length;
      if (tiles_match) {
        for (int i = 0; i < corrected_tiles_length; i++) {
          tiles_match = tiles_match && move->tiles[i] == machine_letters[i];
        }
      }
      free(machine_letters);
      assert(tiles_match);
    }

    if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      assert(move->vertical == vertical);
      assert(move->row_start == move_row_start);
      assert(move->col_start == move_col_start);
    }
  }
}

void test_success_standard() {
  const char *gcg_filename = "success_standard.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert(strings_equal(game_history->title, "test game"));
  assert(strings_equal(game_history->description, "Created with Macondo"));
  assert(strings_equal(game_history->id_auth, "io.woogles"));
  assert(strings_equal(game_history->uid, "p6QRjJHG"));
  assert(strings_equal(game_history->lexicon_name, "CSW21"));
  assert(strings_equal(game_history->letter_distribution_name, "english"));
  assert(game_history->game_variant == GAME_VARIANT_CLASSIC);
  assert(game_history->board_layout == BOARD_LAYOUT_CROSSWORD_GAME);
  assert(strings_equal(game_history->players[0]->nickname, "HastyBot"));
  assert(game_history->players[0]->score == 516);
  assert(!game_history->players[0]->last_known_rack);
  assert(strings_equal(game_history->players[1]->nickname, "RightBehindYou"));
  assert(game_history->players[1]->score == 358);
  assert(!game_history->players[1]->last_known_rack);
  assert(game_history->number_of_events == 29);
  assert_game_event(game_history, 0, GAME_EVENT_EXCHANGE, 0, 0, "DIIIILU", "",
                    GAME_EVENT_EXCHANGE, 0, 0, 0, 0, 5, 6, "IIILU",
                    game_history->letter_distribution);
  assert_game_event(game_history, 1, GAME_EVENT_TILE_PLACEMENT_MOVE, 1, 22,
                    "AAENRSZ", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 7, 6, 22,
                    2, 2, "ZA", game_history->letter_distribution);
  assert_game_event(game_history, 2, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 79,
                    "?CDEIRW", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 8, 4, 79,
                    7, 7, "CRoWDIE", game_history->letter_distribution);
  assert_game_event(game_history, 4, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 149,
                    "?AGNOTT", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 1, 0, 11, 70,
                    7, 8, "TANGOi.T", game_history->letter_distribution);
  assert_game_event(game_history, 7, GAME_EVENT_PASS, 1, 131, "ADGKOSV", "",
                    GAME_EVENT_PASS, 0, 0, 0, 0, 0, 0, "",
                    game_history->letter_distribution);
  assert_game_event(game_history, 9, GAME_EVENT_TILE_PLACEMENT_MOVE, 1, 158,
                    "ADGKOSV", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 5, 1, 27,
                    5, 5, "VOKDA", game_history->letter_distribution);
  assert_game_event(game_history, 10, GAME_EVENT_PHONY_TILES_RETURNED, 1, 131,
                    "ADGKOSV", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 5, 1, 27,
                    5, 5, "VOKDA", game_history->letter_distribution);
  assert_game_event(game_history, 16, GAME_EVENT_PASS, 1, 232, "HLMOORY",
                    "this is a multiline note ", GAME_EVENT_PASS, 0, 0, 0, 0, 0,
                    0, "", game_history->letter_distribution);
  assert_game_event(game_history, 19, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 437,
                    "CEGIIOX", "single line note ",
                    GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 2, 36, 5, 6, "C.IGOE",
                    game_history->letter_distribution);
  assert_game_event(game_history, 27, GAME_EVENT_END_RACK_POINTS, 1, 378, "I",
                    "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0, 0, "",
                    game_history->letter_distribution);
  assert_game_event(game_history, 28, GAME_EVENT_TIME_PENALTY, 1, 358, "", "",
                    GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0, 0, "",
                    game_history->letter_distribution);
  destroy_game_history(game_history);
}

void test_success_five_point_challenge() {
  const char *gcg_filename = "success_five_point_challenge.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert_game_event(game_history, 16, GAME_EVENT_CHALLENGE_BONUS, 1, 398,
                    "DEIINRR", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0,
                    0, 0, "", game_history->letter_distribution);
  assert_game_event(game_history, 23, GAME_EVENT_END_RACK_POINTS, 1, 532,
                    "AGLO", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0,
                    0, "", game_history->letter_distribution);
  assert_game_event(game_history, 24, GAME_EVENT_TIME_PENALTY, 0, 339, "AGLO",
                    "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0, 0, "",
                    game_history->letter_distribution);
  destroy_game_history(game_history);
}

void test_gcg() {
  test_error_cases();
  test_parse_special_char();
  test_parse_special_utf8_no_header();
  test_parse_special_utf8_with_header();
  test_parse_dos_mode();
  test_success_standard();
  test_success_five_point_challenge();
}
