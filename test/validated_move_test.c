#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdlib.h>

void assert_validated_move_error(Game *game, const char *cgp_str,
                                 const char *move_str, int player_index,
                                 bool allow_phonies, bool allow_playthrough,
                                 error_code_t expected_error_status) {
  load_cgp_or_die(game, cgp_str);
  ValidatedMoves *vms = validated_moves_create_and_assert_status(
      game, player_index, move_str, allow_phonies, allow_playthrough,
      expected_error_status);
  validated_moves_destroy(vms);
}

void test_validated_move_errors(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);

  assert_validated_move_error(game, EMPTY_CGP, "", 0, false, false,
                              ERROR_STATUS_MOVE_VALIDATION_EMPTY_MOVE);
  assert_validated_move_error(game, EMPTY_CGP, "          \n  ", 0, false,
                              false, ERROR_STATUS_MOVE_VALIDATION_EMPTY_MOVE);
  assert_validated_move_error(
      game, EMPTY_CGP, "ex ABC", 2, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_PLAYER_INDEX);
  assert_validated_move_error(
      game, EMPTY_CGP, "ex ABC", -1, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_PLAYER_INDEX);
  assert_validated_move_error(
      game, EMPTY_CGP, "ABC", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, " ABC ", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "h8 12345", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_NONEXCHANGE_NUMERIC_TILES);
  assert_validated_move_error(
      game, EMPTY_CGP, "h8 HA#DJI", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILES_PLAYED);
  assert_validated_move_error(
      game, EMPTY_CGP, "h8 HA#DJI", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILES_PLAYED);
  assert_validated_move_error(
      game, EMPTY_CGP, "h8 HADJI", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_NOT_IN_RACK);
  assert_validated_move_error(
      game, ION_OPENING_CGP, "h1 AERATIONS", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_OVERFLOW);
  assert_validated_move_error(
      game, ION_OPENING_CGP, "8A AERATINGS", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_BOARD_MISMATCH);
  assert_validated_move_error(
      game, ION_OPENING_CGP, "h8 COT", 0, false, true,
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_BOARD_MISMATCH);
  assert_validated_move_error(
      game, EMPTY_CGP, "1A QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_DISCONNECTED);
  assert_validated_move_error(
      game, ION_OPENING_CGP, "1A QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_DISCONNECTED);
  assert_validated_move_error(
      game, EMPTY_CGP, "* QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "8 QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "8H1 QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H16 QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "8P QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "P8 QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H8H QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H0H QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "0H0 QAT", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(game, EMPTY_CGP, "ex", 0, false, false,
                              ERROR_STATUS_MOVE_VALIDATION_MISSING_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "h8", 0, false, false,
                              ERROR_STATUS_MOVE_VALIDATION_MISSING_FIELDS);

  assert_validated_move_error(
      game,
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 WECH/ 0/0 0 -lex CSW21;",
      "h8 WECH", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED);
  // Forms AION*
  assert_validated_move_error(
      game, "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 RETAILS/ 0/4 0 ",
      "E5 RETAILS", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED);
  // Forms 7 valid words and 1 phony word:
  // valid: REALISE, RE, EN, AT, LA, IS, SI
  // phony: TS*
  assert_validated_move_error(
      game,
      "15/15/15/15/15/15/15/2ENTASIS6/15/15/15/15/15/15/15 REALIST/ 0/0 0",
      "7C REALIST", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED);

  assert_validated_move_error(
      game, WORMROOT_CGP, "ex BFQR", 0, false, false,
      ERROR_STATUS_MOVE_VALIDATION_EXCHANGE_INSUFFICIENT_TILES);

  assert_validated_move_error(
      game, ION_OPENING_CGP, "8I IZATION", 0, true, true,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_START_COORDS);

  assert_validated_move_error(
      game, ION_OPENING_CGP, "H9 OW", 0, true, true,
      ERROR_STATUS_MOVE_VALIDATION_INVALID_START_COORDS);

  assert_validated_move_error(
      game, ION_OPENING_CGP, "H6 AI", 0, true, true,
      ERROR_STATUS_MOVE_VALIDATION_INCOMPLETE_TILE_PLACEMENT);

  game_destroy(game);
  config_destroy(config);
}

void assert_validated_move_played_tiles(const Config *config, const char *cgp,
                                        const char *ucgi_move,
                                        const char *expected_played_tiles) {
  const LetterDistribution *ld = config_get_ld(config);
  Rack *played_tiles = rack_create(ld_get_size(ld));
  ValidatedMoves *vms = assert_validated_move_success(
      config_get_game(config), cgp, ucgi_move, 0, false, true);
  validated_moves_set_rack_to_played_letters(vms, 0, played_tiles);
  assert_rack_equals_string(ld, played_tiles, expected_played_tiles);
  rack_destroy(played_tiles);
  validated_moves_destroy(vms);
}

void test_validated_move_success(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  const LetterDistribution *ld = config_get_ld(config);
  ValidatedMoves *vms = NULL;
  const Move *move = NULL;
  Rack *leave = rack_create(ld_get_size(ld));
  Rack *played_tiles = rack_create(ld_get_size(ld));

  assert_validated_move_played_tiles(
      config, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 FRAWZEY/ 0/0 0",
      "8H FRAWZEY", "AEFRWYZ");
  assert_validated_move_played_tiles(
      config, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC/ 0/0 0",
      "H8 CAB", "ABC");
  assert_validated_move_played_tiles(
      config, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 INQUZ??/ 0/0 0",
      "8D QUINZe", "INQUZ?");
  assert_validated_move_played_tiles(
      config, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ??/ 0/0 0", "8H qi",
      "??");
  assert_validated_move_played_tiles(
      config, "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 L/ 0/4 0 ",
      "8E L...", "L");
  assert_validated_move_played_tiles(
      config, "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 LS/ 0/4 0 ",
      "8E L...S", "LS");
  assert_validated_move_played_tiles(
      config, "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 Q/ 0/4 0 ",
      "F7 Q.", "Q");
  assert_validated_move_played_tiles(
      config, "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 QS/ 0/4 0 ",
      "F7 Q.S", "QS");
  assert_validated_move_played_tiles(
      config, "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 QI/ 0/4 0 ",
      "7F QI", "IQ");
  assert_validated_move_played_tiles(
      config, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ACENU??/ 0/0 0",
      "8H vAUNCEs", "ACENU??");

  vms = assert_validated_move_success(config_get_game(config), EMPTY_CGP,
                                      "pass", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_PASS);
  assert_move_score(move, 0);
  assert_move_equity_exact(move, EQUITY_PASS_VALUE);
  validated_moves_destroy(vms);

  rack_set_to_string(ld, leave, "ACEGIK");
  vms = assert_validated_move_success(
      config_get_game(config),
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ACEGIK/ 0/0 0", "pass", 0,
      false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_PASS);
  assert_move_score(move, 0);
  assert_move_equity_exact(move, EQUITY_PASS_VALUE);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      config_get_game(config),
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC/ 0/0 0", "ex ABC", 0,
      false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_EXCHANGE);
  assert(move_get_tiles_length(move) == 3);
  assert(move_get_tiles_played(move) == 3);
  assert_move_score(move, 0);
  // Rack is empty, so equity should be zero
  assert_move_equity_exact(move, 0);
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "B"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "C"));
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      config_get_game(config),
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABCDEFG/ 0/0 0", "ex ABC",
      0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_EXCHANGE);
  assert(move_get_tiles_length(move) == 3);
  assert(move_get_tiles_played(move) == 3);
  assert_move_score(move, 0);
  rack_set_to_string(ld, leave, "DEFG");
  assert_move_equity_exact(
      move,
      klv_get_leave_value(
          players_data_get_klv(config_get_players_data(config), 0), leave));
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "B"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "C"));
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      config_get_game(config),
      "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 AAIORT?/ 0/4 0 ",
      "H1 AeRATION", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 8);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_row_start(move) == 0);
  assert(move_get_col_start(move) == 7);
  assert(move_get_dir(move) == BOARD_VERTICAL_DIRECTION);
  assert_move_score(move, 74);
  assert_move_equity_int(move, 74);
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "e"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "R"));
  assert(move_get_tile(move, 3) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 4) == ld_hl_to_ml(ld, "T"));
  assert(move_get_tile(move, 5) == ld_hl_to_ml(ld, "I"));
  assert(move_get_tile(move, 6) == ld_hl_to_ml(ld, "O"));
  assert(move_get_tile(move, 7) == PLAYED_THROUGH_MARKER);
  validated_moves_destroy(vms);

  for (int dir = 0; dir < 2; dir++) {
    if (dir == BOARD_HORIZONTAL_DIRECTION) {
      vms = assert_validated_move_success(
          config_get_game(config),
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ADHIJ/ 0/0 0",
          "8d JIHAD", 0, false, false);
    } else {
      vms = assert_validated_move_success(
          config_get_game(config),
          "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ADHIJ/ 0/0 0",
          "H4 JIHAD", 0, false, false);
    }
    assert(validated_moves_get_number_of_moves(vms) == 1);
    move = validated_moves_get_move(vms, 0);
    assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
    assert(move_get_tiles_length(move) == 5);
    assert(move_get_tiles_played(move) == 5);
    if (dir == BOARD_HORIZONTAL_DIRECTION) {
      assert(move_get_row_start(move) == 7);
      assert(move_get_col_start(move) == 3);
    } else {
      assert(move_get_row_start(move) == 3);
      assert(move_get_col_start(move) == 7);
    }
    assert(move_get_dir(move) == dir);
    assert_move_score(move, 48);
    assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "J"));
    assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "I"));
    assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "H"));
    assert(move_get_tile(move, 3) == ld_hl_to_ml(ld, "A"));
    assert(move_get_tile(move, 4) == ld_hl_to_ml(ld, "D"));
    validated_moves_destroy(vms);
  }

  vms = assert_validated_move_success(
      config_get_game(config),
      "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 AEFFGIR/ 0/4 0 ",
      "H2 FIREFANG", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 8);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_row_start(move) == 1);
  assert(move_get_col_start(move) == 7);
  assert(move_get_dir(move) == BOARD_VERTICAL_DIRECTION);
  assert_move_score(move, 66);
  assert_move_equity_int(move, 66);
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "F"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "I"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "R"));
  assert(move_get_tile(move, 3) == ld_hl_to_ml(ld, "E"));
  assert(move_get_tile(move, 4) == ld_hl_to_ml(ld, "F"));
  assert(move_get_tile(move, 5) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 6) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 7) == ld_hl_to_ml(ld, "G"));
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      config_get_game(config),
      "14E/14N/14d/14U/4GLOWS5R/8PET3E/7FAXING1R/6JAY1TEEMS/2B2BOY4N2/"
      "2L1DOE5U2/"
      "2ANEW5PI2/2MO1LEU3ON2/2EH7HE2/15/15 ?NTT/ 0/0 0 -lex NWL20;",
      "N11 TeNT", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 4);
  assert(move_get_tiles_played(move) == 4);
  assert(move_get_row_start(move) == 10);
  assert(move_get_col_start(move) == 13);
  assert(move_get_dir(move) == BOARD_VERTICAL_DIRECTION);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      config_get_game(config),
      "7N6M/5ZOON4AA/7B5UN/2S4L3LADY/2T4E2QI1I1/2A2PORN3NOR/2BICE2AA1DA1E/"
      "6GUVS1OP1F/8ET1LA1U/5J3R1E1UT/4VOTE1I1R1NE/5G1MICKIES1/6FE1T1THEW/"
      "6OR3E1XI/6OY6G ??DDESW/ 0/0 0 -lex NWL20;",
      "14B hEaDWORDS", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 9);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_row_start(move) == 13);
  assert(move_get_col_start(move) == 1);
  assert(move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION);
  assert_move_score(move, 106);
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "h"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "E"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "a"));
  assert(move_get_tile(move, 3) == ld_hl_to_ml(ld, "D"));
  assert(move_get_tile(move, 4) == ld_hl_to_ml(ld, "W"));
  assert(move_get_tile(move, 5) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 6) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 7) == ld_hl_to_ml(ld, "D"));
  assert(move_get_tile(move, 8) == ld_hl_to_ml(ld, "S"));
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      config_get_game(config),
      "1PACIFYING5/1IS12/YE13/1REQUALIFIED3/H1L12/EDS12/NO3T9/1RAINWASHING3/"
      "UM3O9/T2E1O9/1WAKEnERS6/1OnETIME7/OOT2E1B7/N6U7/1JACULATING4 ABEOPXZ/ "
      "0/0 0 "
      "-lex "
      "NWL20;",
      "A1 OXYPHENBUTAZONE", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 15);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_row_start(move) == 0);
  assert(move_get_col_start(move) == 0);
  assert(move_get_dir(move) == BOARD_VERTICAL_DIRECTION);
  assert_move_score(move, 1780);
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "O"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "X"));
  assert(move_get_tile(move, 2) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 3) == ld_hl_to_ml(ld, "P"));
  assert(move_get_tile(move, 4) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 5) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 6) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 7) == ld_hl_to_ml(ld, "B"));
  assert(move_get_tile(move, 8) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 9) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 10) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 11) == ld_hl_to_ml(ld, "Z"));
  assert(move_get_tile(move, 12) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 13) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 14) == ld_hl_to_ml(ld, "E"));
  validated_moves_destroy(vms);

  // Test allowing playthrough tiles
  // Any number of play through chars is allowed
  vms = assert_validated_move_success(
      config_get_game(config),
      "1PACIFYING5/1IS12/YE13/1REQUALIFIED3/H1L12/EDS12/NO3T9/1RAINWASHING3/"
      "UM3O9/T2E1O9/1WAKEnERS6/1OnETIME7/OOT2E1B7/N6U7/1JACULATING4 ABEOPXZ/ "
      "0/0 0 "
      "-lex "
      "NWL20;",
      "A1 OX.P...B..AZ..E", 0, false, true);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      config_get_game(config),
      "1PACIFYING5/1IS12/YE13/1REQUALIFIED3/H1L12/EDS12/NO3T9/1RAINWASHING3/"
      "UM3O9/T2E1O9/1WAKEnERS6/1OnETIME7/OOT2E1B7/N6U7/1JACULATING4 ABEOPXZ/ "
      "0/0 0 "
      "-lex "
      "NWL20;",
      "A1 OXYPHENBU.AZONE", 0, false, true);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      config_get_game(config),
      "1PACIFYING5/1IS12/YE13/1REQUALIFIED3/H1L12/EDS12/NO3T9/1RAINWASHING3/"
      "UM3O9/T2E1O9/1WAKEnERS6/1OnETIME7/OOT2E1B7/N6U7/1JACULATING4 ABEOPXZ/ "
      "0/0 0 "
      "-lex "
      "NWL20;",
      "A1 OX.PHENBU.AZONE", 0, false, true);
  validated_moves_destroy(vms);

  // Form a bunch of phonies which we will allow.
  vms = assert_validated_move_success(config_get_game(config),
                                      "15/15/15/15/15/15/15/2ENTASIS6/15/15/15/"
                                      "15/15/15/15 VRRUWIW/ 0/0 0 -lex CSW21",
                                      "7C VRRUWIW", 0, true, false);
  validated_moves_destroy(vms);

  // Test equity
  load_and_exec_config_or_die(config, "set -s1 equity");
  // Load a cgp to update the players data
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  vms = assert_validated_move_success(
      config_get_game(config),
      "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 NONAEIR/ 0/4 0 "
      "-lex "
      "CSW21;",
      "9G NON", 0, false, false);
  move = validated_moves_get_move(vms, 0);
  assert_move_score(move, 10);
  rack_set_to_string(ld, leave, "AEIR");
  assert_move_equity_exact(
      move,
      move_get_score(move) +
          klv_get_leave_value(
              players_data_get_klv(config_get_players_data(config), 0), leave));
  validated_moves_destroy(vms);

  load_and_exec_config_or_die(config, "set -s1 score");
  // Load a cgp to update the players data
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  vms = assert_validated_move_success(
      config_get_game(config),
      "15/15/15/15/15/15/15/5ION7/15/15/15/15/15/15/15 NON/ 0/4 0 "
      "-lex "
      "CSW21;",
      "9g NON", 0, false, false);
  move = validated_moves_get_move(vms, 0);
  assert_move_score(move, 10);
  assert_move_equity_int(move, 10);
  validated_moves_destroy(vms);

  load_and_exec_config_or_die(config, "set -ld english_blank_is_5");
  // Load a cgp load the game
  load_and_exec_config_or_die(config, "cgp " EMPTY_CGP);
  vms = assert_validated_move_success(
      config_get_game(config),
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ?ENAIDA/ 0/0 0 -lex CSW21;",
      "8D zENAIDA", 0, false, false);
  move = validated_moves_get_move(vms, 0);
  assert_move_score(move, 84);
  validated_moves_destroy(vms);

  rack_destroy(played_tiles);
  rack_destroy(leave);
  config_destroy(config);
}

void test_validated_move_score(void) {
  // The validated move scoring uses different
  // code than move generation, so it must be tested
  // separately.
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  ValidatedMoves *vms = NULL;

  vms = assert_validated_move_success(game, ION_OPENING_CGP, "7f IEE", 0, true,
                                      false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  assert_move_score(validated_moves_get_move(vms, 0), 11);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(game,
                                      "15/15/3THERMOS2A2/15/15/15/15/15/15/15/"
                                      "15/15/15/15/15 HITNE/ 0/0 0 -lex CSW21;",
                                      "3B HITHERMOST,3B NETHERMOST", 0, false,
                                      false);
  assert(validated_moves_get_number_of_moves(vms) == 2);
  assert_move_score(validated_moves_get_move(vms, 0), 36);
  assert_move_score(validated_moves_get_move(vms, 1), 30);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      game,
      "14E/14N/14d/14U/4GLOWS5R/8PET3E/7FAXING1R/6JAY1TEEMS/2B2BOY4N2/"
      "2L1DOE5U2/"
      "2ANEW5PI2/2MO1LEU3ON2/2EH7HE2/15/15 AEIR/ 0/0 0 -lex NWL20;",
      " 5B AIRGLOWS  , 5c     REGLOWS   ", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 2);
  assert_move_score(validated_moves_get_move(vms, 0), 12);
  assert_move_score(validated_moves_get_move(vms, 1), 11);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      game,
      "7ZEP1F3/1FLUKY3R1R3/5EX2A1U3/2SCARIEST1I3/9TOT3/6GO1LO4/6OR1ETA3/"
      "6JABS1b3/"
      "5QI4A3/5I1N3N3/3ReSPOND1D3/1HOE3V3O3/1ENCOMIA3N3/7T7/3VENGED6 ABEDLT/ "
      "0/0 0 "
      "-lex "
      "NWL20;",
      "15c AVENGED,1L FA,K9 TAEL,b10 BEHEAD,K9 "
      "TAE,K11 ED,K10 AE,K11 ETA,B9 BATHED",
      0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 9);
  assert_move_score(validated_moves_get_move(vms, 0), 12);
  assert_move_score(validated_moves_get_move(vms, 1), 5);
  assert_move_score(validated_moves_get_move(vms, 2), 38);
  assert_move_score(validated_moves_get_move(vms, 3), 36);
  assert_move_score(validated_moves_get_move(vms, 4), 34);
  assert_move_score(validated_moves_get_move(vms, 5), 33);
  assert_move_score(validated_moves_get_move(vms, 6), 30);
  assert_move_score(validated_moves_get_move(vms, 7), 34);
  assert_move_score(validated_moves_get_move(vms, 8), 28);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      game,
      "7N6M/5ZOON4AA/7B5UN/2S4L3LADY/2T4E2QI1I1/2A2PORN3NOR/2BICE2AA1DA1E/"
      "6GUVS1OP1F/8ET1LA1U/5J3R1E1UT/4VOTE1I1R1NE/5G1MICKIES1/6FE1T1THEW/"
      "6OR3E1XI/6OY6G ??DDESW/ 0/0 0 -lex NWL20;",
      "14B hEaDWORDS,14B hEaDWORD", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 2);
  assert_move_score(validated_moves_get_move(vms, 0), 106);
  assert_move_score(validated_moves_get_move(vms, 1), 38);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      game,
      "1PACIFYING5/1IS12/YE13/1REQUALIFIED3/H1L12/EDS12/NO3T9/1RAINWASHING3/"
      "UM3O9/T2E1O9/1WAKEnERS6/1OnETIME7/OOT2E1B7/N6U7/1JACULATING4 ABEOPXZ/ "
      "0/0 0 "
      "-lex "
      "NWL20;",
      "A1 OXYPHENBUTAZONE,A12 ZONE", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 2);
  assert_move_score(validated_moves_get_move(vms, 0), 1780);
  assert_move_score(validated_moves_get_move(vms, 1), 160);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      game,
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 OVERDOG/ 0/0 0 -lex CSW21;",
      "8C OVERDOG", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  assert_move_score(validated_moves_get_move(vms, 0), 82);
  validated_moves_destroy(vms);

  game_destroy(game);
  config_destroy(config);
}

void test_validated_move_distinct_kwg(void) {
  Config *config =
      config_create_or_die("set -l1 CSW21 -l2 NWL20 -s1 equity -s2 equity "
                           "-r1 best -r2 best -numplays 1");
  Game *game = config_game_create(config);
  MoveList *move_list = move_list_create(1);
  const MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  // Play SPORK, better than best NWL move of PORKS
  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);
  draw_rack_string_from_bag(game, 0, "KOPRRSS");
  ValidatedMoves *vms = validated_moves_create_and_assert_status(
      game, 0, "8H SPORK", false, false, ERROR_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  generate_moves_for_game(&move_gen_args);
  assert_move(game, move_list, NULL, 0, "8H SPORK 32");
  play_move(move_list_get_move(move_list, 0), game, NULL);

  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);
  draw_rack_string_from_bag(game, 1, "CEHIIRZ");
  // Play SCHIZIER, better than best CSW word of SCHERZI
  vms = validated_moves_create_and_assert_status(game, 1, "H8 SCHIZIER", false,
                                                 false, ERROR_STATUS_SUCCESS);
  assert_move_score(validated_moves_get_move(vms, 0), 146);
  validated_moves_destroy(vms);

  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);
  draw_rack_string_from_bag(game, 1, "CEHIRSZ");
  vms = validated_moves_create_and_assert_status(
      game, 1, "M8 SCHERZI", false, false,
      ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);
  draw_rack_string_from_bag(game, 1, "CEHIIRZ");
  generate_moves_for_game(&move_gen_args);
  assert_move(game, move_list, NULL, 0, "H8 (S)CHIZIER 146");
  play_move(move_list_get_move(move_list, 0), game, NULL);

  // Play WIGGLY, not GOLLYWOG because that's NWL only
  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);
  draw_rack_string_from_bag(game, 0, "GGLLOWY");
  vms = validated_moves_create_and_assert_status(game, 0, "11G WIGGLY", false,
                                                 false, ERROR_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  vms = validated_moves_create_and_assert_status(
      game, 0, "J2 GOLLYWOG", false, false,
      ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  generate_moves_for_game(&move_gen_args);
  assert_move(game, move_list, NULL, 0, "11G W(I)GGLY 28");
  play_move(move_list_get_move(move_list, 0), game, NULL);

  // Play 13C QUEAS(I)ER, not L3 SQUEA(K)ER(Y) because that's CSW only
  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);
  draw_rack_string_from_bag(game, 1, "AEEQRSU");
  vms = validated_moves_create_and_assert_status(game, 1, "13C QUEASIER", false,
                                                 false, ERROR_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  vms = validated_moves_create_and_assert_status(
      game, 1, "L3 SQUEAKERY", false, false,
      ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  generate_moves_for_game(&move_gen_args);
  assert_move(game, move_list, NULL, 0, "13C QUEAS(I)ER 88");

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void test_validated_move_wordsmog_phonies(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21_alpha -wmp false -s1 equity -s2 equity "
      "-r1 best -r2 best -numplays 1 -var wordsmog");
  Game *game = config_game_create(config);

  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);
  draw_rack_string_from_bag(game, 0, "TRONGLE");
  ValidatedMoves *vms = validated_moves_create_and_assert_status(
      game, 0, "8H TRONGLE", false, false,
      ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  load_cgp_or_die(game, ENTASIS_OPENING_CGP);

  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);
  draw_rack_string_from_bag(game, 0, "DUORGNA");
  vms = validated_moves_create_and_assert_status(
      game, 0, "7C DUOGRNA", false, false,
      ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  return_rack_to_bag(game, 0);
  return_rack_to_bag(game, 1);
  draw_rack_string_from_bag(game, 0, "DUORENA");
  vms = validated_moves_create_and_assert_status(game, 0, "7C DUORENA", false,
                                                 false, ERROR_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  game_destroy(game);
  config_destroy(config);
}

void test_validated_move_many(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);

  ValidatedMoves *vms = assert_validated_move_success(
      game,
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 JIHADQS/ 0/0 0 -lex CSW21;",
      "  pass ,   ex ADJ,  8d JIHAD , h8 QIS ", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 4);
  assert(move_get_type(validated_moves_get_move(vms, 0)) == GAME_EVENT_PASS);
  assert(move_get_type(validated_moves_get_move(vms, 1)) ==
         GAME_EVENT_EXCHANGE);
  assert(move_get_type(validated_moves_get_move(vms, 2)) ==
         GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_type(validated_moves_get_move(vms, 3)) ==
         GAME_EVENT_TILE_PLACEMENT_MOVE);
  validated_moves_destroy(vms);

  draw_rack_string_from_bag(game, 0, "UVV");
  vms = validated_moves_create_and_assert_status(
      game, 0, "pass UVV,ex UVV ,8h VVU", false, false,
      ERROR_STATUS_MOVE_VALIDATION_EXCESS_PASS_FIELDS);
  validated_moves_destroy(vms);

  vms = validated_moves_create_and_assert_status(
      game, 0, " pass, ex ABC,8h VVU", false, false,
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_NOT_IN_RACK);
  validated_moves_destroy(vms);

  vms = validated_moves_create_and_assert_status(
      game, 0, " pass, ex uUV,8h VVU", false, false,
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_NOT_IN_RACK);
  validated_moves_destroy(vms);

  vms = validated_moves_create_and_assert_status(
      game, 0, "  pass ,  ex UVV , 8h VVU", false, false,
      ERROR_STATUS_MOVE_VALIDATION_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  game_destroy(game);
  config_destroy(config);
}

void test_validated_move(void) {
  test_validated_move_errors();
  test_validated_move_success();
  test_validated_move_score();
  test_validated_move_distinct_kwg();
  test_validated_move_wordsmog_phonies();
  test_validated_move_many();
}
