#include "../src/def/board_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/game_history.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/gcg.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *parse_and_write_gcg(const char *gcg_filepath_read,
                          const char *gcg_filepath_write, Config *config,
                          GameHistory *game_history, ErrorStack *error_stack) {
  config_parse_gcg(config, gcg_filepath_read, game_history, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to parse gcg: %s\n", gcg_filepath_read);
  }

  write_gcg(gcg_filepath_write, config_get_ld(config), game_history,
            error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to write gcg: %s\n", gcg_filepath_write);
  }

  char *gcg_string = get_string_from_file(gcg_filepath_write, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to get gcg string: %s\n", gcg_filepath_write);
  }

  return gcg_string;
}

void assert_gcg_write_cycle(const char *gcg_filename_readonly, Config *config,
                            GameHistory *game_history) {
  ErrorStack *error_stack = error_stack_create();

  char *gcg_filepath_read = data_filepaths_get_readable_filename(
      config_get_data_paths(config), gcg_filename_readonly,
      DATA_FILEPATH_TYPE_GCG, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    log_fatal("failed to get filepath for gcg: %s\n", gcg_filename_readonly);
  }

  char *gcg_filepath_write1 = insert_before_dot(gcg_filepath_read, "_write1");
  char *iter_1_gcg_string =
      parse_and_write_gcg(gcg_filepath_read, gcg_filepath_write1, config,
                          game_history, error_stack);

  char *gcg_filepath_write2 = insert_before_dot(gcg_filepath_read, "_write2");
  char *iter_2_gcg_string =
      parse_and_write_gcg(gcg_filepath_write1, gcg_filepath_write2, config,
                          game_history, error_stack);

  if (!strings_equal(iter_1_gcg_string, iter_2_gcg_string)) {
    log_fatal("gcg strings are not equal for file '%s'\n", gcg_filepath_read);
  }
  free(iter_1_gcg_string);
  free(iter_2_gcg_string);
  free(gcg_filepath_read);
  free(gcg_filepath_write1);
  free(gcg_filepath_write2);
  error_stack_destroy(error_stack);
}

void test_single_error_case(const char *gcg_filename, Config *config,
                            GameHistory *game_history,
                            error_code_t expected_gcg_parse_status) {
  printf("testing error case for %s\n", gcg_filename);
  error_code_t gcg_parse_status =
      test_parse_gcg(gcg_filename, config, game_history);
  const bool ok = gcg_parse_status == expected_gcg_parse_status;
  if (!ok) {
    printf("gcg parse status mismatched:\nexpected: %d\nactual: %d\n>%s<\n",
           expected_gcg_parse_status, gcg_parse_status, gcg_filename);
    assert(0);
  }
}

void test_game_history(GameHistory *game_history) {
  // Check that whitespace is trimmed from the name
  game_history_player_reset_names(game_history, 0,
                                  " \n  \r  first  last\n  \n\r\n", NULL);
  assert_strings_equal(game_history_player_get_name(game_history, 0),
                       "first  last");
  assert_strings_equal(game_history_player_get_nickname(game_history, 0),
                       "first__last");

  // Check that the GCG extension is properly added
  game_history_set_gcg_filename(game_history,
                                "player_one_vs_player_two" GCG_EXTENSION);
  assert_strings_equal(game_history_get_gcg_filename(game_history),
                       "player_one_vs_player_two" GCG_EXTENSION);

  game_history_set_gcg_filename(game_history, "a" GCG_EXTENSION);
  assert_strings_equal(game_history_get_gcg_filename(game_history),
                       "a" GCG_EXTENSION);

  game_history_set_gcg_filename(game_history, "b");
  assert_strings_equal(game_history_get_gcg_filename(game_history),
                       "b" GCG_EXTENSION);

  game_history_set_gcg_filename(game_history, "some_game");
  assert_strings_equal(game_history_get_gcg_filename(game_history),
                       "some_game" GCG_EXTENSION);
}

void test_error_cases(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  test_single_error_case("empty", config, game_history,
                         ERROR_STATUS_GCG_PARSE_GCG_EMPTY);
  test_single_error_case("unsupported_character_encoding", config, game_history,
                         ERROR_STATUS_GCG_PARSE_UNSUPPORTED_CHARACTER_ENCODING);
  test_single_error_case("duplicate_nicknames", config, game_history,
                         ERROR_STATUS_GCG_PARSE_DUPLICATE_NICKNAMES);
  test_single_error_case("duplicate_names", config, game_history,
                         ERROR_STATUS_GCG_PARSE_DUPLICATE_NAMES);
  test_single_error_case("description_after_events", config, game_history,
                         ERROR_STATUS_GCG_PARSE_PRAGMA_SUCCEEDED_EVENT);
  test_single_error_case("move_before_player", config, game_history,
                         ERROR_STATUS_GCG_PARSE_GAME_EVENT_BEFORE_PLAYER);
  test_single_error_case("player_number_redundant", config, game_history,
                         ERROR_STATUS_GCG_PARSE_PLAYER_NUMBER_REDUNDANT);
  test_single_error_case("encoding_wrong_place", config, game_history,
                         ERROR_STATUS_GCG_PARSE_MISPLACED_ENCODING);
  test_single_error_case("player_does_not_exist", config, game_history,
                         ERROR_STATUS_GCG_PARSE_PLAYER_DOES_NOT_EXIST);
  test_single_error_case("note_before_events", config, game_history,
                         ERROR_STATUS_GCG_PARSE_NOTE_PRECEDENT_EVENT);
  test_single_error_case(
      "phony_tiles_returned_with_no_play", config, game_history,
      ERROR_STATUS_GCG_PARSE_PHONY_TILES_RETURNED_WITHOUT_PLAY);
  test_single_error_case("no_matching_token", config, game_history,
                         ERROR_STATUS_GCG_PARSE_NO_MATCHING_TOKEN);
  test_single_error_case("invalid_tile_placement", config, game_history,
                         ERROR_STATUS_GCG_PARSE_MOVE_VALIDATION_ERROR);
  test_single_error_case("malformed_rack", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_MALFORMED);
  test_single_error_case("six_pass_last_rack_malformed", config, game_history,
                         ERROR_STATUS_GCG_PARSE_PLAYED_LETTERS_NOT_IN_RACK);
  test_single_error_case("exchange_not_in_rack", config, game_history,
                         ERROR_STATUS_GCG_PARSE_MOVE_VALIDATION_ERROR);
  test_single_error_case("exchange_malformed", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_MALFORMED);
  test_single_error_case("pass_rack_malformed", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_MALFORMED);
  test_single_error_case("challenge_bonus_rack_malformed", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_MALFORMED);
  test_single_error_case("end_points_rack_malformed", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_MALFORMED);
  test_single_error_case("end_penalty_rack_malformed", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_MALFORMED);
  test_single_error_case("malformed_play", config, game_history,
                         ERROR_STATUS_GCG_PARSE_MOVE_VALIDATION_ERROR);
  test_single_error_case("played_tile_not_in_rack", config, game_history,
                         ERROR_STATUS_GCG_PARSE_MOVE_VALIDATION_ERROR);
  test_single_error_case("play_out_of_bounds", config, game_history,
                         ERROR_STATUS_GCG_PARSE_MOVE_VALIDATION_ERROR);
  test_single_error_case("redundant_pragma", config, game_history,
                         ERROR_STATUS_GCG_PARSE_REDUNDANT_PRAGMA);
  test_single_error_case("unrecognized_game_variant", config, game_history,
                         ERROR_STATUS_GCG_PARSE_UNRECOGNIZED_GAME_VARIANT);
  test_single_error_case("last_rack_penalty_incorrect", config, game_history,
                         ERROR_STATUS_GCG_PARSE_END_RACK_PENALTY_INCORRECT);
  test_single_error_case("end_rack_points_incorrect", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_END_POINTS_INCORRECT);
  test_single_error_case("six_pass_cumulative_scoring_error", config,
                         game_history,
                         ERROR_STATUS_GCG_PARSE_CUMULATIVE_SCORING_ERROR);
  test_single_error_case("standard_cumulative_scoring_error", config,
                         game_history,
                         ERROR_STATUS_GCG_PARSE_CUMULATIVE_SCORING_ERROR);
  test_single_error_case("game_event_off_turn", config, game_history,
                         ERROR_STATUS_GCG_PARSE_GAME_EVENT_OFF_TURN);
  test_single_error_case("move_event_after_game_end", config, game_history,
                         ERROR_STATUS_GCG_PARSE_MOVE_EVENT_AFTER_GAME_END);
  test_single_error_case("challenge_bonus_without_play", config, game_history,
                         ERROR_STATUS_GCG_PARSE_CHALLENGE_BONUS_WITHOUT_PLAY);
  test_single_error_case(
      "invalid_challenge_bonus_player_index", config, game_history,
      ERROR_STATUS_GCG_PARSE_INVALID_CHALLENGE_BONUS_PLAYER_INDEX);
  test_single_error_case(
      "invalid_phony_tiles_player_index", config, game_history,
      ERROR_STATUS_GCG_PARSE_INVALID_PHONY_TILES_PLAYER_INDEX);
  test_single_error_case("phony_tiles_returned_mismatch", config, game_history,
                         ERROR_STATUS_GCG_PARSE_PHONY_TILES_RETURNED_MISMATCH);
  test_single_error_case("end_game_event_before_game_end", config, game_history,
                         ERROR_STATUS_GCG_PARSE_END_GAME_EVENT_BEFORE_GAME_END);
  test_single_error_case("first_rack_not_in_bag", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG);
  test_single_error_case("midgame_rack_not_in_bag", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG);
  test_single_error_case("event_after_last_rack", config, game_history,
                         ERROR_STATUS_GCG_PARSE_EVENT_AFTER_LAST_RACK);
  test_single_error_case("last_rack1_not_in_bag", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG);
  test_single_error_case("last_rack2_not_in_bag", config, game_history,
                         ERROR_STATUS_GCG_PARSE_RACK_NOT_IN_BAG);
  test_single_error_case("time_penalty_first_event", config, game_history,
                         ERROR_STATUS_GCG_PARSE_END_GAME_EVENT_BEFORE_GAME_END);
  test_single_error_case("time_penalty_midgame", config, game_history,
                         ERROR_STATUS_GCG_PARSE_END_GAME_EVENT_BEFORE_GAME_END);
  test_single_error_case("time_penalty_before_rack_points", config,
                         game_history,
                         ERROR_STATUS_GCG_PARSE_PREMATURE_TIME_PENALTY);
  test_single_error_case("time_penalty_between_rack_penalties", config,
                         game_history,
                         ERROR_STATUS_GCG_PARSE_PREMATURE_TIME_PENALTY);
  test_single_error_case("time_penalty_before_rack_penalties", config,
                         game_history,
                         ERROR_STATUS_GCG_PARSE_PREMATURE_TIME_PENALTY);
  test_single_error_case("time_penalty_duplicate", config, game_history,
                         ERROR_STATUS_GCG_PARSE_GAME_REDUNDANT_TIME_PENALTY);
  config_destroy(config);

  Config *no_lexicon_config = config_create_or_die(
      "set -s1 equity -s2 equity -r1 all -r2 all -numplays 1");

  test_single_error_case("lexicon_missing", no_lexicon_config, game_history,
                         ERROR_STATUS_GCG_PARSE_LEXICON_NOT_SPECIFIED);

  config_destroy(no_lexicon_config);
}

void test_parse_special_char(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const char *gcg_filename = "name_iso8859-1";
  error_code_t gcg_parse_status =
      test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  assert_strings_equal(game_history_player_get_name(game_history, 0), "césar");
  assert_strings_equal(game_history_player_get_name(game_history, 1),
                       "hércules");

  config_destroy(config);
}

void test_parse_special_utf8_no_header(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const char *gcg_filename = "name_utf8_noheader";
  error_code_t gcg_parse_status =
      test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  assert(
      strings_equal(game_history_player_get_name(game_history, 0), "cÃ©sar"));

  config_destroy(config);
}

void test_parse_special_utf8_with_header(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const char *gcg_filename = "name_utf8_with_header";
  error_code_t gcg_parse_status =
      test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  assert(strings_equal(game_history_player_get_name(game_history, 0), "césar"));

  config_destroy(config);
}

void test_parse_dos_mode(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const char *gcg_filename = "utf8_dos";
  error_code_t gcg_parse_status =
      test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  assert(strings_equal(game_history_player_get_nickname(game_history, 0),
                       "angwantibo"));
  assert(strings_equal(game_history_player_get_nickname(game_history, 1),
                       "Michal_Josko"));

  config_destroy(config);
}

void assert_game_event(const GameHistory *game_history, int event_index,
                       game_event_t event_type, int player_index,
                       int cumulative_score, const char *rack_string,
                       const char *note, game_event_t move_type, int dir,
                       int move_row_start, int move_col_start, int move_score,
                       int tiles_played, int tiles_length,
                       const char *tiles_string, int score_adjustment,
                       const LetterDistribution *ld) {
  const GameEvent *game_event =
      game_history_get_event(game_history, event_index);
  int ld_size = ld_get_size(ld);
  // Game Event assertions
  assert(game_event_get_type(game_event) == event_type);
  assert(game_event_get_player_index(game_event) == player_index);
  assert(game_event_get_cumulative_score(game_event) ==
         int_to_equity(cumulative_score));
  assert(game_event_get_score_adjustment(game_event) ==
         int_to_equity(score_adjustment));
  Rack expected_rack;
  rack_set_dist_size_and_reset(&expected_rack, ld_size);
  bool racks_match = false;
  if (string_length(rack_string) > 0) {
    rack_set_to_string(ld, &expected_rack, rack_string);
  }
  racks_match =
      racks_are_equal(&expected_rack, game_event_get_const_rack(game_event));

  assert(racks_match);
  if (!((!game_event_get_note(game_event) && string_length(note) == 0) ||
        strings_equal(game_event_get_note(game_event), note))) {
    log_fatal("notes not equal:\nactual:   >%s<\nexpected: >%s<\n",
              game_event_get_note(game_event), note);
  }

  // Move assertions
  const ValidatedMoves *vms = game_event_get_vms(game_event);
  const Move *move = NULL;
  if (vms) {
    move = validated_moves_get_move(vms, 0);
  }
  if (move) {
    assert(move_get_type(move) == move_type);
    assert_move_score(move, move_score);

    if (move_type != GAME_EVENT_PASS) {
      assert(move_get_tiles_played(move) == tiles_played);
      assert(move_get_tiles_length(move) == tiles_length);
      MachineLetter *machine_letters =
          malloc_or_die(sizeof(MachineLetter) * tiles_length);
      int number_of_machine_letters = ld_str_to_mls(
          ld, tiles_string, move_type == GAME_EVENT_TILE_PLACEMENT_MOVE,
          machine_letters, tiles_length);
      bool tiles_match = number_of_machine_letters == tiles_length;
      if (tiles_match) {
        for (int i = 0; i < tiles_length; i++) {
          tiles_match =
              tiles_match && move_get_tile(move, i) == machine_letters[i];
        }
      }
      free(machine_letters);
      assert(tiles_match);
    }

    if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      assert(move_get_dir(move) == dir);
      assert(move_get_row_start(move) == move_row_start);
      assert(move_get_col_start(move) == move_col_start);
    }
  }
}

void assert_game_play_n_events(GameHistory *game_history, Game *game1,
                               Game *game2, const char **cgps) {
  int event_index = 0;
  const char *cgp = cgps[event_index];
  while (cgp) {
    game_play_n_events_or_die(game_history, game1, event_index);
    load_cgp_or_die(game2, cgp);
    assert_games_are_equal(game1, game2, true);
    event_index++;
    cgp = cgps[event_index];
  }
}

void test_success_standard(GameHistory *game_history) {
  Config *config = config_create_or_die("set -lex CSW21 -s1 equity -s2 equity "
                                        "-r1 all -r2 all -numplays 1");
  const char *gcg_filename = "success_standard";
  error_code_t gcg_parse_status =
      test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);

  Game *game1 = config_game_create(config);
  Game *game2 = config_game_create(config);
  const LetterDistribution *ld = config_get_ld(config);
  Rack rack;
  memset(&rack, 0, sizeof(rack));

  assert(strings_equal(game_history_get_title(game_history), "test game"));
  assert(strings_equal(game_history_get_description(game_history),
                       "Created with Macondo"));
  assert(strings_equal(game_history_get_id_auth(game_history), "io.woogles"));
  assert(strings_equal(game_history_get_uid(game_history), "p6QRjJHG"));
  assert(strings_equal(game_history_get_lexicon_name(game_history), "CSW21"));
  assert(strings_equal(game_history_get_ld_name(game_history), "english"));
  assert(game_history_get_game_variant(game_history) == GAME_VARIANT_CLASSIC);
  assert(strings_equal(game_history_get_board_layout_name(game_history),
                       "standard15"));
  assert(strings_equal(game_history_player_get_nickname(game_history, 0),
                       "HastyBot"));
  assert(racks_are_equal(game_history_player_get_last_rack(game_history, 0),
                         &rack));
  assert(strings_equal(game_history_player_get_nickname(game_history, 1),
                       "RightBehindYou"));

  assert(racks_are_equal(game_history_player_get_last_rack(game_history, 1),
                         &rack));

  assert(game_history_get_num_events(game_history) == 29);
  assert_game_event(game_history, 0, GAME_EVENT_EXCHANGE, 0, 0, "DIIIILU", "",
                    GAME_EVENT_EXCHANGE, 0, 0, 0, 0, 5, 5, "IIILU", 0, ld);
  assert_game_event(game_history, 1, GAME_EVENT_TILE_PLACEMENT_MOVE, 1, 22,
                    "AAENRSZ", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 7, 6, 22,
                    2, 2, "ZA", 0, ld);
  assert_game_event(game_history, 2, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 79,
                    "?CDEIRW", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 8, 4, 79,
                    7, 7, "CRoWDIE", 0, ld);
  assert_game_event(game_history, 4, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 149,
                    "?AGNOTT", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 1, 0, 11, 70,
                    7, 8, "TANGOi.T", 0, ld);
  assert_game_event(game_history, 7, GAME_EVENT_PASS, 1, 131, "ADGKOSV", "",
                    GAME_EVENT_PASS, 0, 0, 0, 0, 0, 0, "", 0, ld);
  assert_game_event(game_history, 9, GAME_EVENT_TILE_PLACEMENT_MOVE, 1, 158,
                    "ADGKOSV", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 5, 1, 27,
                    5, 5, "VOKDA", 0, ld);
  assert_game_event(game_history, 10, GAME_EVENT_PHONY_TILES_RETURNED, 1, 131,
                    "ADGKOSV", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 5, 1, 27,
                    5, 5, "VOKDA", 0, ld);
  assert_game_event(game_history, 16, GAME_EVENT_PASS, 1, 232, "HLMOORY",
                    "this\nis\na\nmultiline\nnote\n", GAME_EVENT_PASS, 0, 0, 0,
                    0, 0, 0, "", 0, ld);
  assert_game_event(game_history, 19, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 437,
                    "CEGIIOX", "single line note\n",
                    GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 2, 36, 5, 6, "C.IGOE",
                    0, ld);
  assert_game_event(game_history, 27, GAME_EVENT_END_RACK_POINTS, 1, 378, "I",
                    "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0, 0, "", 2,
                    ld);
  assert_game_event(game_history, 28, GAME_EVENT_TIME_PENALTY, 1, 358, "", "",
                    GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0, 0, "", -20,
                    ld);

  assert_game_play_n_events(
      game_history, game1, game2,
      (const char *[]){
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DIIIILU/ 0/0 0",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AAENRSZ/ 0/0 1",
          "15/15/15/15/15/15/15/6ZA7/15/15/15/15/15/15/15 CDEIRW?/ 0/22 0",
          "15/15/15/15/15/15/15/6ZA7/4CRoWDIE4/15/15/15/15/15/15 AENNRST/ "
          "22/79 0",
          "15/15/15/15/15/15/5TANNERS3/6ZA7/4CRoWDIE4/15/15/15/15/15/15 "
          "AGNOTT?/ 79/99 0",
          "11T3/11A3/11N3/11G3/11O3/11i3/5TANNERS3/6ZA3T3/4CRoWDIE4/15/15/15/"
          "15/15/15 BEEFKOS/ 99/149 0",
          "11T3/11A3/11N3/11G3/11O3/11i3/5TANNERS3/6ZA3T3/4CRoWDIE4/9FEEB2/15/"
          "15/15/15/15 EEIMNTU/ 149/131 0",
          "11T3/10NA3/10EN3/10UG3/10MO3/11i3/5TANNERS3/6ZA3T3/4CRoWDIE4/9FEEB2/"
          "15/15/15/15/15 ADGKOSV/ 131/176 0",
          "11T3/10NA3/10EN3/10UG3/10MO3/11i3/5TANNERS3/6ZA3T3/4CRoWDIE4/9FEEB2/"
          "15/15/15/15/15 AAEEILT/ 176/131 1",
          "11T3/10NA3/10EN3/10UG3/10MO3/11i3/5TANNERS3/6ZA3T3/4CRoWDIE4/9FEEB2/"
          "10LEA2/15/15/15/15 ADGKOSV/ 131/194 0",
          "11T3/10NA3/10EN3/10UG3/10MO3/1VOKDA5i3/5TANNERS3/6ZA3T3/4CRoWDIE4/"
          "9FEEB2/10LEA2/15/15/15/15 ADEEIRT/ 194/158 0",
          "11T3/10NA3/10EN3/10UG3/10MO3/11i3/5TANNERS3/6ZA3T3/4CRoWDIE4/9FEEB2/"
          "10LEA2/15/15/15/15 ADEEIRT/ADKOV 194/131 1",
          "11T3/10NAE2/10END2/10UG3/10MO3/11i3/5TANNERS3/6ZA3T3/4CRoWDIE4/"
          "9FEEB2/10LEA2/15/15/15/15 ADGKOSV/ 131/211 0",
          "11T3/10NAE2/10END2/10UG3/10MO3/1VODKA5i3/5TANNERS3/6ZA3T3/4CRoWDIE4/"
          "9FEEB2/10LEA2/15/15/15/15 ABEIRTV/ 211/158 0",
          "11T3/10NAE2/10END2/2A7UG3/2B7MO3/1VODKA5i3/2R2TANNERS3/2T3ZA3T3/"
          "2I1CRoWDIE4/2V6FEEB2/2E7LEA2/15/15/15/15 AGILORS/ 158/276 0",
          "11T3/10NAE2/10END2/2A7UG1G1/2B7MO1L1/1VODKA5i1O1/2R2TANNERS1R1/"
          "2T3ZA3T1I1/2I1CRoWDIE2A1/2V6FEEBS1/2E7LEA2/15/15/15/15 HILNSWY/ "
          "276/232 0",
          "11T3/10NAE2/10END2/2A7UG1G1/2B7MO1L1/1VODKA5i1O1/2R2TANNERS1R1/"
          "2T3ZA3T1I1/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/15/15/15/15 "
          "HLMOORY/ 232/360 0",
          "11T3/10NAE2/10END2/2A7UG1G1/2B7MO1L1/1VODKA5i1O1/2R2TANNERS1R1/"
          "2T3ZA3T1I1/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/15/15/15/15 "
          "CDEEFGO/ 360/232 1",
          "11T3/10NAE2/10END2/2A7UG1G1/2B7MO1L1/1VODKA5i1OD/2R2TANNERS1RE/"
          "2T3ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/15/15/15/15 "
          "HLMOORY/ 232/401 0",
          "3H7T3/3O6NAE2/3M6END2/2AY6UG1G1/2B7MO1L1/1VODKA5i1OD/2R2TANNERS1RE/"
          "2T3ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/15/15/15/15 "
          "CEGIIOX/ 401/274 0",
          "2CHIGOE3T3/3O6NAE2/3M6END2/2AY6UG1G1/2B7MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/2T3ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/15/"
          "15/15/15 AILOORU/ 274/437 0",
          "2CHIGOE3T3/3O6NAE2/3M6END2/2AY6UG1G1/2B7MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
          "15/15/15/15 AEEIJNX/ 437/289 0",
          "2CHIGOE3T3/3O6NAE2/3M6END2/2AY6UG1G1/2B7MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
          "12N2/12J2/12A2/12X2 ILOOPQR/ 289/481 0",
          "2CHIGOE3T3/3O6NAE2/3M6END2/2AY6UG1G1/2B7MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
          "12N2/12J2/12A2/7PROLIX2 EEIIPRU/IOQSTTU 481/337 0",
          "2CHIGOE3T2P/3O6NAE1E/3M6END1R/2AY6UG1GI/2B7MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
          "12N2/12J2/12A2/7PROLIX2 IOQSTTU/EIU 337/506 0",
          "2CHIGOE3T2P/3O6NAE1E/3M6END1R/2AY6UG1GI/2B2QUIT1MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
          "12N2/12J2/12A2/7PROLIX2 EIU/OST 506/362 0",
          "2CHIGOE3T2P/3O6NAE1E/3M6END1R/2AY6UG1GI/2B2QUIT1MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
          "12N2/12JEU/12A2/7PROLIX2 OST/I 362/516 0",
          "2CHIGOE3T2P/3O6NAE1E/3M6END1R/2AY6UG1GI/2B2QUIT1MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
          "3O8N2/3T8JEU/3S8A2/7PROLIX2 I/ 516/376 0",
          "2CHIGOE3T2P/3O6NAE1E/3M6END1R/2AY6UG1GI/2B2QUIT1MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
          "3O8N2/3T8JEU/3S8A2/7PROLIX2 I/ 516/378 0",
          "2CHIGOE3T2P/3O6NAE1E/3M6END1R/2AY6UG1GI/2B2QUIT1MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
          "3O8N2/3T8JEU/3S8A2/7PROLIX2 I/ 516/358 0",
          "2CHIGOE3T2P/3O6NAE1E/3M6END1R/2AY6UG1GI/2B2QUIT1MO1L1/1VODKA5i1OD/"
          "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/"
          "3O8N2/3T8JEU/3S8A2/7PROLIX2 I/ 516/358 0",
          // Mark the end of the list
          NULL,
      });

  game_destroy(game1);
  game_destroy(game2);
  config_destroy(config);
}

void test_success_five_point_challenge(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const char *gcg_filename = "success_five_point_challenge";
  error_code_t gcg_parse_status =
      test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);

  Game *game1 = config_game_create(config);
  Game *game2 = config_game_create(config);

  const LetterDistribution *ld = config_get_ld(config);
  assert_game_event(game_history, 16, GAME_EVENT_CHALLENGE_BONUS, 1, 398,
                    "DEIINRR", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0,
                    0, 0, "", 5, ld);
  assert_game_event(game_history, 23, GAME_EVENT_END_RACK_POINTS, 1, 532,
                    "AGLO", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0,
                    0, "", 10, ld);
  assert_game_event(game_history, 24, GAME_EVENT_TIME_PENALTY, 0, 339, "AGLO",
                    "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0, 0, 0, "",
                    -30, ld);

  assert_game_play_n_events(
      game_history, game1, game2,
      (const char *[]){
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 OOQSUY?/ 0/0 0",
          "15/15/15/15/15/15/15/6SUQ6/15/15/15/15/15/15/15 CDEEHTV/ 0/24 0",
          "15/15/15/15/15/15/15/6SUQ6/2VETCH8/15/15/15/15/15/15 FIOOTY?/ "
          "24/30 0",
          "15/15/15/15/15/15/15/6SUQ6/2VETCH8/6YOOF5/15/15/15/15/15 "
          "BDDEELO/ 30/51 0",
          "15/15/15/15/15/15/15/3B2SUQ6/2VETCH8/3D2YOOF5/3E11/3L11/15/15/15 "
          "AEINNT?/ 51/52 0",
          "15/15/15/15/10I4/10N4/10A4/3B2SUQ1N4/2VETCH3E4/3D2YOOFs4/3E6T4/3L11/"
          "15/15/15 ADEILMO/ 52/135 0",
          "15/15/15/15/10I4/10N4/10A4/3B2SUQ1N4/2VETCH3E4/3D2YOOFs4/3E6T4/3L11/"
          "1MELODIA7/15/15 AEEIIKR/ 135/133 0",
          "15/15/15/15/10I4/10N4/10A4/3B2SUQ1N4/2VETCH3E4/1K1D2YOOFs4/1E1E6T4/"
          "1R1L11/1MELODIA7/1A13/15 ACHRSU?/ 133/177 0",
          "15/15/15/15/10I4/10N4/10A4/3B2SUQ1N4/2VETCH3E4/1K1D2YOOFs4/1E1E6T4/"
          "1R1L11/1MELODIA7/1A13/1SCRAUcH7 EEEFIIN/ 177/231 0",
          "15/15/15/15/10I4/10N4/10A4/3B2SUQ1N4/2VETCH3E4/1K1D2YOOFs4/1E1E6T4/"
          "1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 GNPRRWZ/ 231/196 0",
          "15/15/15/15/10I4/10N4/9WARP2/3B2SUQ1N4/2VETCH3E4/1K1D2YOOFs4/"
          "1E1E6T4/1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 ABEEIIN/ 196/243 0",
          "15/15/15/15/4B5I4/4I5N4/4N4WARP2/3BA1SUQ1N4/2VETCH3E4/1K1DE1YOOFs4/"
          "1E1E6T4/1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 GLMNNRZ/ 243/219 0",
          "15/15/15/15/4B5I4/2GRIZ4N4/4N4WARP2/3BA1SUQ1N4/2VETCH3E4/"
          "1K1DE1YOOFs4/1E1E6T4/1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 "
          "AEIIOST/ 219/277 0",
          "15/15/15/15/4B5I4/2GRIZ2ION4/4N4WARP2/3BA1SUQ1N4/2VETCH3E4/"
          "1K1DE1YOOFs4/1E1E6T4/1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 "
          "EGLMNNX/ 277/231 0",
          "15/15/15/15/4B4MINX2/2GRIZ2ION4/4N4WARP2/3BA1SUQ1N4/2VETCH3E4/"
          "1K1DE1YOOFs4/1E1E6T4/1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 "
          "AAEISTY/ 231/298 0",
          "15/15/15/12AY1/4B4MINX2/2GRIZ2ION4/4N4WARP2/3BA1SUQ1N4/2VETCH3E4/"
          "1K1DE1YOOFs4/1E1E6T4/1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 "
          "AEGILNT/ 298/245 0",
          "15/15/8GENITAL/12AY1/4B4MINX2/2GRIZ2ION4/4N4WARP2/3BA1SUQ1N4/"
          "2VETCH3E4/1K1DE1YOOFs4/1E1E6T4/1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 "
          "AEIOOST/ 245/393 0",
          "15/15/8GENITAL/12AY1/4B4MINX2/2GRIZ2ION4/4N4WARP2/3BA1SUQ1N4/"
          "2VETCH3E4/1K1DE1YOOFs4/1E1E6T4/1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 "
          "AEIOOST/ 245/398 0",
          "15/15/8GENITAL/12AY1/4B4MINX2/2GRIZ2ION4/4N4WARP2/3BA1SUQ1N4/"
          "2VETCH3E4/OK1DE1YOOFs4/OE1E6T4/1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 "
          "DEIINRR/ 398/255 0",
          NULL,
      });

  game_destroy(game1);
  game_destroy(game2);
  config_destroy(config);
}

void test_success_six_pass(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const char *gcg_filename = "success_six_pass";
  error_code_t gcg_parse_status =
      test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);

  Game *game1 = config_game_create(config);
  Game *game2 = config_game_create(config);

  const LetterDistribution *ld = config_get_ld(config);
  assert_game_event(game_history, 0, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 80,
                    "AEEGTTV", "", GAME_EVENT_TILE_PLACEMENT_MOVE,
                    BOARD_HORIZONTAL_DIRECTION, 7, 1, 80, 7, 7, "GAVETTE", 0,
                    ld);
  assert_game_event(game_history, 1, GAME_EVENT_PHONY_TILES_RETURNED, 0, 0,
                    "AEEGTTV", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0,
                    0, 0, "GAVETTE", 0, ld);
  assert_game_event(
      game_history, 2, GAME_EVENT_EXCHANGE, 1, 0, "DEIIKOR",
      "for some dumb reason, I thought vegetate was spelled with an I, so I "
      "traded this instead of playing it. I even said \"you're not getting an "
      "I,\" which led to Marlon's next play.\n",
      GAME_EVENT_EXCHANGE, 0, 0, 0, 0, 3, 3, "KOI", 0, ld);
  assert_game_event(game_history, 3, GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 80,
                    "AEEGTTV", "", GAME_EVENT_TILE_PLACEMENT_MOVE,
                    BOARD_HORIZONTAL_DIRECTION, 7, 3, 80, 7, 7, "VETTAGE", 0,
                    ld);
  assert_game_event(game_history, 4, GAME_EVENT_PHONY_TILES_RETURNED, 0, 0,
                    "AEEGTTV", "", GAME_EVENT_TILE_PLACEMENT_MOVE, 0, 0, 0, 0,
                    0, 0, "VETTAGE", 0, ld);
  assert_game_event(game_history, 5, GAME_EVENT_EXCHANGE, 1, 0, "DDEIRRT",
                    "I still think that he needs an I. Specifying Marlon's "
                    "rack, dirt wins the sim by a mile.\n",
                    GAME_EVENT_EXCHANGE, 0, 0, 0, 0, 2, 2, "DR", 0, ld);
  assert_game_event(
      game_history, 6, GAME_EVENT_EXCHANGE, 0, 0, "AGEETTV",
      "When he said \"exchange 2\" I knew exactly what he was trying to do, "
      "and decided that I would have an easier time trying to beat him in a "
      "coin flip than I would in a game of scrabble, since he's a better "
      "player than me. My only regret is not taking enough time to think about "
      "what I was going to do.\n",
      GAME_EVENT_EXCHANGE, 0, 0, 0, 0, 2, 2, "VG", 0, ld);
  assert_game_event(game_history, 7, GAME_EVENT_EXCHANGE, 1, 0, "DEGIRRT",
                    "okay marlon, let's see what you've got...\n",
                    GAME_EVENT_EXCHANGE, 0, 0, 0, 0, 2, 2, "DG", 0, ld);
  assert_game_event(
      game_history, 8, GAME_EVENT_END_RACK_PENALTY, 0, -6, "?AEEOTT",
      "Woah! He drew the blank! But I still need to draw my own tiles... I can "
      "still win if I draw a 1 point tile and a blank myself.\n",
      GAME_EVENT_END_RACK_PENALTY, 0, 0, 0, 0, 0, 0, "", -6, ld);
  assert_game_event(
      game_history, 9, GAME_EVENT_END_RACK_PENALTY, 1, -16, "EIOQRRT",
      "well... so much for that... The game ended here with the score of -16 "
      "for me and -6 for Marlon. This ties for the lowest tournament losing "
      "score ever... Plus I'm not even upset that it didn't work out. It is "
      "worth it just for the story (in my opinion), and I'm going to be "
      "telling this one for years to come.\n",
      GAME_EVENT_END_RACK_PENALTY, 0, 0, 0, 0, 0, 0, "", -16, ld);

  assert_game_play_n_events(
      game_history, game1, game2,
      (const char *[]){
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEEGTTV/ 0/0 0",
          "15/15/15/15/15/15/15/1GAVETTE7/15/15/15/15/15/15/15 DEIIKOR/ 0/80 0",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DEIIKOR/AEEGTTV 0/0 1",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEEGTTV/ 0/0 2",
          "15/15/15/15/15/15/15/3VETTAGE5/15/15/15/15/15/15/15 DDEIRRT/ 0/80 0",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DDEIRRT/AEEGTTV 0/0 3",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEEGTTV/ 0/0 4",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DEGIRRT/ 0/0 5",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEEOTT?/ 0/0 6",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 EIOQRRT/ 0/-6 6",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / -6/-16 6",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / -6/-16 6",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / -6/-16 6",
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / -6/-16 6",
          NULL,
      });

  game_destroy(game1);
  game_destroy(game2);
  config_destroy(config);
}

void test_success_incomplete(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");

  Game *game1 = config_game_create(config);
  Game *game2 = config_game_create(config);
  const LetterDistribution *ld = config_get_ld(config);
  Rack rack;
  const char *gcg_filename;
  error_code_t gcg_parse_status;

  gcg_filename = "oops_all_pragmas";
  gcg_parse_status = test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_to_end_or_die(game_history, game1);
  load_cgp_or_die(game2,
                  "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0");
  assert_games_are_equal(game1, game2, true);
  memset(&rack, 0, sizeof(rack));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 0)));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 1)));

  gcg_filename = "success_just_last_rack";
  gcg_parse_status = test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_to_end_or_die(game_history, game1);
  load_cgp_or_die(
      game2, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DIIIILU/ 0/0 0");
  assert_games_are_equal(game1, game2, true);
  rack_set_to_string(ld, &rack, "DIIIILU");
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 0)));
  memset(&rack, 0, sizeof(rack));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 1)));

  gcg_filename = "incomplete_after_tile_placement";
  gcg_parse_status = test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_to_end_or_die(game_history, game1);
  load_cgp_or_die(game2,
                  "15/15/15/15/15/15/15/6ZA7/4CRoWDIE4/15/15/15/15/15/15 / "
                  "22/79 0");
  assert_games_are_equal(game1, game2, true);
  memset(&rack, 0, sizeof(rack));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 0)));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 1)));

  gcg_filename = "incomplete_with_last_rack_after_tile_placement";
  gcg_parse_status = test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_to_end_or_die(game_history, game1);
  load_cgp_or_die(
      game2, "15/15/15/15/15/15/15/6ZA7/4CRoWDIE4/15/15/15/15/15/15 AENNRST/ "
             "22/79 0");
  assert_games_are_equal(game1, game2, true);
  memset(&rack, 0, sizeof(rack));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 0)));
  rack_set_to_string(ld, &rack, "AENNRST");
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 1)));

  gcg_filename = "incomplete_after_five_point_challenge";
  gcg_parse_status = test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_n_events_or_die(game_history, game1, 2);
  load_cgp_or_die(
      game2,
      "15/15/15/15/15/15/15/6SUQ6/2VETCH8/15/15/15/15/15/15 ?FIOOTY/ 24/30 0");
  assert_games_are_equal(game1, game2, true);
  rack_set_to_string(ld, &rack, "AEIOOST");
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 0)));
  memset(&rack, 0, sizeof(rack));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 1)));

  gcg_filename = "incomplete_after_phony_returned";
  gcg_parse_status = test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_to_end_or_die(game_history, game1);
  load_cgp_or_die(
      game2,
      "11T3/10NA3/10EN3/10UG3/10MO3/11i3/5TANNERS3/6ZA3T3/4CRoWDIE4/9FEEB2/"
      "10LEA2/15/15/15/15 ADEEIRT/ADKOV 194/131 1");
  assert_games_are_equal(game1, game2, true);
  rack_set_to_string(ld, &rack, "ADEEIRT");
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 0)));
  memset(&rack, 0, sizeof(rack));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 1)));

  gcg_filename = "incomplete_after_pass";
  gcg_parse_status = test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_to_end_or_die(game_history, game1);
  load_cgp_or_die(
      game2, "11T3/10NAE2/10END2/2A7UG1G1/2B7MO1L1/1VODKA5i1O1/2R2TANNERS1R1/"
             "2T3ZA3T1I1/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/15/15/15/15 "
             "CDEEFGO/ 360/232 1");
  assert_games_are_equal(game1, game2, true);
  rack_set_to_string(ld, &rack, "CDEEFGO");
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 0)));
  memset(&rack, 0, sizeof(rack));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 1)));

  gcg_filename = "incomplete_after_five_point_challenge";
  gcg_parse_status = test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_to_end_or_die(game_history, game1);
  load_cgp_or_die(
      game2,
      "15/15/8GENITAL/12AY1/4B4MINX2/2GRIZ2ION4/4N4WARP2/3BA1SUQ1N4/"
      "2VETCH3E4/1K1DE1YOOFs4/1E1E6T4/1R1L3FIE5/1MELODIA7/1A13/1SCRAUcH7 "
      "AEIOOST/ 245/398 0");
  assert_games_are_equal(game1, game2, true);
  rack_set_to_string(ld, &rack, "AEIOOST");
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 0)));
  memset(&rack, 0, sizeof(rack));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 1)));

  gcg_filename = "incomplete_after_exchange";
  gcg_parse_status = test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_to_end_or_die(game_history, game1);
  load_cgp_or_die(
      game2, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AAENRSZ/ 0/0 1");
  assert_games_are_equal(game1, game2, true);
  memset(&rack, 0, sizeof(rack));
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 0)));
  rack_set_to_string(ld, &rack, "AAENRSZ");
  assert(racks_are_equal(&rack,
                         game_history_player_get_last_rack(game_history, 1)));
  game_destroy(game1);
  game_destroy(game2);
  config_destroy(config);
}

void test_success_phony_empty_bag(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");

  Game *game1 = config_game_create(config);
  Game *game2 = config_game_create(config);

  assert(test_parse_gcg("phony_with_empty_bag", config, game_history) ==
         ERROR_STATUS_SUCCESS);

  game_play_n_events_or_die(game_history, game1, 25);
  load_cgp_or_die(
      game2,
      "2CHIGOE3T3/3O6NAE2/3M6END2/2AY6UG1G1/2B7MO1L1/1VODKA5i1OD/2R2TANNERS1RE/"
      "OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/1NEWISHLY1LEA2/12N2/12J2/12A2/"
      "7PROLIX2 IOQSTTU/EEIIPRU 337/481 1");
  assert_games_are_equal(game1, game2, true);

  game_play_n_events_or_die(game_history, game1, 26);
  load_cgp_or_die(
      game2, "2CHIGOE3T3/3O6NAE2/3M6END2/2AY6UG1G1/2B7MO1L1/1VODKA5i1OD/"
             "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/"
             "1NEWISHLY1LEA2/12N2/12J2/11QATS/7PROLIX2 EEIIPRU/IOTU 481/374 0");
  assert_games_are_equal(game1, game2, true);

  game_play_n_events_or_die(game_history, game1, 31);
  load_cgp_or_die(
      game2, "2CHIGOE3T2P/3O6NAE1U/3M6END1R/2AY6UG1GI/2B7MO1L1/1VODKA5i1OD/"
             "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/1IV6FEEBS1/"
             "1NEWISHLY1LEA2/12N2/12J2/7OUT1QATS/7PROLIX2 EEI/ 506/400 0");
  assert_games_are_equal(game1, game2, true);

  game_play_n_events_or_die(game_history, game1, 32);
  load_cgp_or_die(
      game2, "2CHIGOE3T2P/3O6NAE1U/3M6END1R/2AY6UG1GI/2B7MO1L1/1VODKA5i1OD/"
             "2R2TANNERS1RE/OUTA2ZA3T1IF/2I1CRoWDIE2A1/2V6FEEBS1/"
             "1NEWISHLY1LEA2/12N2/12J2/7OUT1QATS/7PROLIX2 EEI/I 506/389 1");
  assert_games_are_equal(game1, game2, true);

  game_destroy(game1);
  game_destroy(game2);
  config_destroy(config);
}

void test_success_out_in_many(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");

  const LetterDistribution *ld = config_get_ld(config);
  Game *game1 = config_game_create(config);
  Game *game2 = config_game_create(config);

  assert(test_parse_gcg("out_in_many", config, game_history) ==
         ERROR_STATUS_SUCCESS);

  // Ensure that the racks are set correctly
  game_play_n_events_or_die(game_history, game1, 22);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 0)), "");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 1)),
                            "DEOX");

  game_play_n_events_or_die(game_history, game1, 23);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 0)),
                            "BIJSSUV");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 1)),
                            "ABLN");

  game_play_n_events_or_die(game_history, game1, 24);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 0)),
                            "JSU");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 1)),
                            "ABLN");

  game_play_n_events_or_die(game_history, game1, 25);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 0)),
                            "JSU");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 1)),
                            "N");

  game_play_n_events_or_die(game_history, game1, 26);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 0)),
                            "S");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 1)),
                            "N");

  game_play_n_events_or_die(game_history, game1, 27);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 0)),
                            "S");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game1, 1)), "");

  game_destroy(game1);
  game_destroy(game2);
  config_destroy(config);
}

void test_success_long_game(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");

  Game *game = config_game_create(config);
  const LetterDistribution *ld = config_get_ld(config);
  Rack *rack = rack_create(ld_get_size(ld));
  const char *gcg_filename;
  error_code_t gcg_parse_status;

  gcg_filename = "success_long_game";
  gcg_parse_status = test_parse_gcg(gcg_filename, config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_to_end_or_die(game_history, game);

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void test_write_gcg(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");

  assert_gcg_write_cycle("success", config, game_history);
  assert_gcg_write_cycle("success_standard", config, game_history);
  assert_gcg_write_cycle("success_five_point_challenge", config, game_history);
  assert_gcg_write_cycle("success_just_last_rack", config, game_history);
  assert_gcg_write_cycle("success_six_pass", config, game_history);

  config_destroy(config);
}

void test_partially_known_rack_from_phonies(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");

  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);

  error_code_t gcg_parse_status =
      test_parse_gcg("partially_known_rack_from_phonies", config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  game_play_n_events_or_die(game_history, game, 2);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "DEEORUV");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)), "");

  game_play_n_events_or_die(game_history, game, 5);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "BDFLQRU");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)),
                            "AAAEIST");

  game_play_n_events_or_die(game_history, game, 6);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)), "");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)),
                            "AAAEIST");

  game_play_n_events_or_die(game_history, game, 7);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "BELNQRU");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)),
                            "AAAEST");

  game_play_n_events_or_die(game_history, game, 8);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)), "");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)),
                            "AAAELST");

  game_play_n_events_or_die(game_history, game, 9);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "AHINNRW");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)),
                            "AEST");

  game_play_n_events_or_die(game_history, game, 11);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "AEEHIMN");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)),
                            "AST");

  game_play_n_events_or_die(game_history, game, 14);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "AEHIMNR");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)),
                            "AILRST");

  game_play_n_events_or_die(game_history, game, 15);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)), "");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)),
                            "AAILRST");

  game_play_n_events_or_die(game_history, game, 16);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "EINNNX?");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)), "L");

  game_play_n_events_or_die(game_history, game, 18);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "EFINNN?");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)), "L");

  game_play_n_events_or_die(game_history, game, 21);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "AGILNO?");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)),
                            "AAGJLOO");

  // After an exchange, we cannot be sure of any of the opponent's tiles
  game_play_n_events_or_die(game_history, game, 23);
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 0)),
                            "EEIOSVY");
  assert_rack_equals_string(ld, player_get_rack(game_get_player(game, 1)), "");

  game_destroy(game);
  config_destroy(config);
}

void test_vs_jeremy_gcg(GameHistory *game_history) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  error_code_t gcg_parse_status =
      test_parse_gcg("vs_jeremy", config, game_history);
  assert(gcg_parse_status == ERROR_STATUS_SUCCESS);
  config_destroy(config);
}

void test_gcg(void) {
  // Use the same game_history for all tests to thoroughly test the
  // game_history_reset function
  GameHistory *game_history = game_history_create();
  test_game_history(game_history);
  test_error_cases(game_history);
  test_parse_special_char(game_history);
  test_parse_special_utf8_no_header(game_history);
  test_parse_special_utf8_with_header(game_history);
  test_parse_dos_mode(game_history);
  test_success_standard(game_history);
  test_success_five_point_challenge(game_history);
  test_success_six_pass(game_history);
  test_success_incomplete(game_history);
  test_success_phony_empty_bag(game_history);
  test_success_out_in_many(game_history);
  test_vs_jeremy_gcg(game_history);
  test_write_gcg(game_history);
  test_partially_known_rack_from_phonies(game_history);
  game_history_destroy(game_history);
}
