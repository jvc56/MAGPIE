#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/gcg_defs.h"

#include "../src/ent/game_history.h"
#include "../src/impl/gcg.h"
#include "../src/util/util.h"

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
  GameHistoryPlayer *player0 = get_game_history_player(game_history, 0);
  GameHistoryPlayer *player1 = get_game_history_player(game_history, 1);
  assert_strings_equal(get_game_history_player_name(player0), "césar");
  assert_strings_equal(get_game_history_player_name(player1), "hércules");
  destroy_game_history(game_history);
}

void test_parse_special_utf8_no_header() {
  const char *gcg_filename = "name_utf8_noheader.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  GameHistoryPlayer *player0 = get_game_history_player(game_history, 0);
  assert(strings_equal(get_game_history_player_name(player0), "cÃ©sar"));
  destroy_game_history(game_history);
}

void test_parse_special_utf8_with_header() {
  const char *gcg_filename = "name_utf8_with_header.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  GameHistoryPlayer *player0 = get_game_history_player(game_history, 0);
  assert(strings_equal(get_game_history_player_name(player0), "césar"));
  destroy_game_history(game_history);
}

void test_parse_dos_mode() {
  const char *gcg_filename = "utf8_dos.gcg";
  GameHistory *game_history = create_game_history();
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  GameHistoryPlayer *player0 = get_game_history_player(game_history, 0);
  GameHistoryPlayer *player1 = get_game_history_player(game_history, 1);
  assert(
      strings_equal(get_game_history_player_nickname(player0), "angwantibo"));
  assert(
      strings_equal(get_game_history_player_nickname(player1), "Michal_Josko"));
  destroy_game_history(game_history);
}

void assert_game_event(const GameHistory *game_history, int event_index,
                       game_event_t event_type, int player_index,
                       int cumulative_score, const char *rack_string,
                       const char *note, game_event_t move_type, int dir,
                       int move_row_start, int move_col_start, int move_score,
                       int tiles_played, int tiles_length,
                       const char *tiles_string,
                       const LetterDistribution *letter_distribution) {
  GameEvent *game_event = get_game_history_event(game_history, event_index);
  int ld_size = letter_distribution_get_size(letter_distribution);
  // Game Event assertions
  assert(get_game_event_event_type(game_event) == event_type);
  assert(get_game_event_player_index(game_event) == player_index);
  assert(get_game_event_cumulative_score(game_event) == cumulative_score);
  Rack *expected_rack = NULL;
  bool racks_match = false;
  if (string_length(rack_string) > 0) {
    expected_rack = create_rack(ld_size);
    set_rack_to_string(letter_distribution, expected_rack, rack_string);
    racks_match =
        racks_are_equal(expected_rack, get_game_event_rack(game_event));
    destroy_rack(expected_rack);
  } else {
    racks_match =
        racks_are_equal(expected_rack, get_game_event_rack(game_event));
  }

  assert(racks_match);
  assert((!get_game_event_note(game_event) && string_length(note) == 0) ||
         strings_equal(get_game_event_note(game_event), note));

  // Move assertions
  const Move *move = get_game_event_move(game_event);
  if (move) {

    assert(get_move_type(move) == move_type);
    assert(get_score(move) == move_score);

    if (move_type != GAME_EVENT_PASS) {
      assert(get_tiles_played(move) == tiles_played);
      assert(get_tiles_length(move) == tiles_length);
      uint8_t *machine_letters = malloc_or_die(sizeof(uint8_t) * tiles_length);
      int number_of_machine_letters = str_to_machine_letters(
          get_game_history_letter_distribution(game_history), tiles_string,
          move_type == GAME_EVENT_TILE_PLACEMENT_MOVE, machine_letters,
          tiles_length);
      bool tiles_match = number_of_machine_letters == tiles_length;
      if (tiles_match) {
        for (int i = 0; i < tiles_length; i++) {
          tiles_match = tiles_match && get_tile(move, i) == machine_letters[i];
        }
      }
      free(machine_letters);
      assert(tiles_match);
    }

    if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      assert(get_dir(move) == dir);
      assert(get_row_start(move) == move_row_start);
      assert(get_col_start(move) == move_col_start);
    }
  }
}

void test_success_standard() {
  const char *gcg_filename = "success_standard.gcg";
  GameHistory *game_history = create_game_history();
  const LetterDistribution *ld =
      get_game_history_letter_distribution(game_history);

  Player *player0 = get_game_history_player(game_history, 0);
  Player *player1 = get_game_history_player(game_history, 1);

  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert(strings_equal(get_game_history_title(game_history), "test game"));
  assert(strings_equal(get_game_history_description(game_history),
                       "Created with Macondo"));
  assert(strings_equal(get_game_history_id_auth(game_history), "io.woogles"));
  assert(strings_equal(get_game_history_uid(game_history), "p6QRjJHG"));
  assert(strings_equal(get_game_history_lexicon_name(game_history), "CSW21"));
  assert(strings_equal(get_game_history_letter_distribution_name(game_history),
                       "english"));
  assert(get_game_history_game_variant(game_history) == GAME_VARIANT_CLASSIC);
  assert(get_game_history_board_layout(game_history) ==
         BOARD_LAYOUT_CROSSWORD_GAME);
  assert(strings_equal(get_game_history_player_nickname(player0), "HastyBot"));
  assert(get_game_history_player_score(player0) == 516);
  assert(!get_game_history_player_last_known_rack(player0));
  assert(strings_equal(get_game_history_player_nickname(player1),
                       "RightBehindYou"));
  assert(get_game_history_player_score(player1) == 358);
  assert(!get_game_history_player_last_known_rack(player1));

  assert(get_game_history_number_of_events(game_history) == 29);
  assert_game_event(game_history, 0, GAME_EVENT_EXCHANGE, 0, 0, "DIIIILU", "",
                    GAME_EVENT_EXCHANGE, 0, 0, 0, 0, 5, 5, "IIILU", ld);
  assert_game_event(game_history, 1, GAME_EVENT_TILE_PLACEMENT_MOVE, 1, 22,
                    "AAENRSZ", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 7, 6, 22,
                    2, 2, "ZA", ld);
  assert_game_event(game_history, 2, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 79,
                    "?CDEIRW", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 8, 4, 79,
                    7, 7, "CRoWDIE", ld);
  assert_game_event(game_history, 4, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 149,
                    "?AGNOTT", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 1, 0, 11, 70,
                    7, 8, "TANGOi.T", ld);
  assert_game_event(game_history, 7, GAME_EVENT_PASS, 1, 131, "ADGKOSV", "",
                    GAME_EVENT_PASS, 0, 0, 0, 0, 0, 0, "", ld);
  assert_game_event(game_history, 9, GAME_EVENT_TILE_PLACEMENT_MOVE, 1, 158,
                    "ADGKOSV", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 5, 1, 27,
                    5, 5, "VOKDA", ld);
  assert_game_event(game_history, 10, GAME_EVENT_PHONY_TILES_RETURNED, 1, 131,
                    "ADGKOSV", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 5, 1, 27,
                    5, 5, "VOKDA", ld);
  assert_game_event(game_history, 16, GAME_EVENT_PASS, 1, 232, "HLMOORY",
                    "this is a multiline note ", GAME_EVENT_PASS, 0, 0, 0, 0, 0,
                    0, "", ld);
  assert_game_event(game_history, 19, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 437,
                    "CEGIIOX", "single line note ",
                    GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 2, 36, 5, 6, "C.IGOE",
                    ld);
  assert_game_event(game_history, 27, GAME_EVENT_END_RACK_POINTS, 1, 378, "I",
                    "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0, 0, "",
                    ld);
  assert_game_event(game_history, 28, GAME_EVENT_TIME_PENALTY, 1, 358, "", "",
                    GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0, 0, "", ld);
  destroy_game_history(game_history);
}

void test_success_five_point_challenge() {
  const char *gcg_filename = "success_five_point_challenge.gcg";
  GameHistory *game_history = create_game_history();
  const LetterDistribution *ld =
      get_game_history_letter_distribution(game_history);
  gcg_parse_status_t gcg_parse_status =
      test_parse_gcg(gcg_filename, game_history);
  assert(gcg_parse_status == GCG_PARSE_STATUS_SUCCESS);
  assert_game_event(game_history, 16, GAME_EVENT_CHALLENGE_BONUS, 1, 398,
                    "DEIINRR", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0,
                    0, 0, "", ld);
  assert_game_event(game_history, 23, GAME_EVENT_END_RACK_POINTS, 1, 532,
                    "AGLO", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0,
                    0, "", ld);
  assert_game_event(game_history, 24, GAME_EVENT_TIME_PENALTY, 0, 339, "AGLO",
                    "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0, 0, "",
                    ld);
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
