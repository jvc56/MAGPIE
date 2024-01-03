#include <assert.h>
#include <stdio.h>

#include "../src/ent/config.h"
#include "../src/ent/game.h"
#include "game_test.h"
#include "rack_test.h"
#include "test_constants.h"
#include "test_util.h"

void reset_and_load_game_success(Game *game, const char *cgp) {
  cgp_parse_status_t cgp_parse_status = game_load_cgp(game, cgp);
  assert(cgp_parse_status == CGP_PARSE_STATUS_SUCCESS);
}

void reset_and_load_game_failure(Game *game, const char *cgp,
                                 cgp_parse_status_t expected_cgp_parse_status) {
  cgp_parse_status_t cgp_parse_status = game_load_cgp(game, cgp);
  assert(cgp_parse_status == expected_cgp_parse_status);
}

void test_load_cgp() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  // Test that loading various CGPs doesn't result in
  // any errors
  reset_and_load_game_success(game, EMPTY_CGP);
  reset_and_load_game_success(game, EMPTY_PLAYER0_RACK_CGP);
  reset_and_load_game_success(game, EMPTY_PLAYER1_RACK_CGP);
  reset_and_load_game_success(game, OPENING_CGP);
  reset_and_load_game_success(game, DOUG_V_EMELY_DOUBLE_CHALLENGE_CGP);
  reset_and_load_game_success(game, DOUG_V_EMELY_CGP);
  reset_and_load_game_success(game, GUY_VS_BOT_ALMOST_COMPLETE_CGP);
  reset_and_load_game_success(game, GUY_VS_BOT_CGP);
  reset_and_load_game_success(game, INCOMPLETE_3_CGP);
  reset_and_load_game_success(game, INCOMPLETE4_CGP);
  reset_and_load_game_success(game, INCOMPLETE_ELISE_CGP);
  reset_and_load_game_success(game, INCOMPLETE_CGP);
  reset_and_load_game_success(game, JOSH2_CGP);
  reset_and_load_game_success(game, NAME_ISO8859_1_CGP);
  reset_and_load_game_success(game, NAME_UTF8_NOHEADER_CGP);
  reset_and_load_game_success(game, NAME_UTF8_WITH_HEADER_CGP);
  reset_and_load_game_success(game, NOAH_VS_MISHU_CGP);
  reset_and_load_game_success(game, NOAH_VS_PETER_CGP);
  reset_and_load_game_success(game, SOME_ISC_GAME_CGP);
  reset_and_load_game_success(game, UTF8_DOS_CGP);
  reset_and_load_game_success(game, VS_ANDY_CGP);
  reset_and_load_game_success(game, VS_FRENTZ_CGP);

  // Empty string
  reset_and_load_game_failure(game, "",
                              CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS);
  reset_and_load_game_failure(game, "           ",
                              CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS);
  reset_and_load_game_failure(game, "\n\r\t\v\f  ",
                              CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS);
  // Missing board
  reset_and_load_game_failure(game, "/ 0/0 0",
                              CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS);
  // Missing racks
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 0/0 0",
      CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS);
  // Missing scores
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0",
      CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS);

  // Missing consecutive zeros
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0",
      CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS);

  // Too many board rows
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0",
      CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_ROWS);

  // Not enough columns
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/AB3E4F3/15/15/15/15/15/15 / 0/0 0",
      CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_COLUMNS);

  // Not enough columns
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/14 / 0/0 0",
      CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_COLUMNS);

  // Too many columns
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/AB3CDE4F3/15/15/15/15/15/15 / 0/0 0",
      CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_COLUMNS);

  // Too many columns
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/16 / 0/0 0",
      CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_COLUMNS);

  // Too many racks
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC/DEF/GHI 0/0 0",
      CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_RACKS);

  // Too few racks
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC 0/0 0",
      CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_RACKS);

  // Too many scores
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC/DEF 123/456/78 0",
      CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_SCORES);

  // Too few scores
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC/DEF 9000 0",
      CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_SCORES);

  // Invalid board letters
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/ABCD.EFG7/15/15/15/15/15/15 ABC/DEF 0/0 0",
      CGP_PARSE_STATUS_MALFORMED_BOARD_LETTERS);

  // Invalid rack letters
  reset_and_load_game_failure(
      game,
      "15/15/15/15/15/15/15/15/3ABCDEFG5/15/15/15/15/15/15 ABC5DF/YXZ 0/0 0",
      CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS);

  // Invalid scores
  reset_and_load_game_failure(game,
                              "15/15/15/15/15/15/15/15/5ABCDEFG3/15/15/15/15/"
                              "15/15 ABCDF/YXZ 234R3/34 0",
                              CGP_PARSE_STATUS_MALFORMED_SCORES);

  // Invalid consecutive zeros
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABCDF/YXZ 0/0 H",
      CGP_PARSE_STATUS_MALFORMED_CONSECUTIVE_ZEROS);

  game_destroy(game);
  config_destroy(config);
}

void test_game_main() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  const LetterDistribution *ld = config_get_letter_distribution(config);
  Game *game = game_create(config);
  Rack *rack = create_rack(letter_distribution_get_size(ld));
  cgp_parse_status_t cgp_parse_status;

  Rack *player0_rack = player_get_rack(game_get_player(game, 0));
  Rack *player1_rack = player_get_rack(game_get_player(game, 1));

  // Test Reset
  game_set_consecutive_scoreless_turns(game, 3);
  game_set_game_end_reason(game, GAME_END_REASON_STANDARD);
  game_reset(game);
  assert(game_get_consecutive_scoreless_turns(game) == 0);
  assert(game_get_game_end_reason(game) == GAME_END_REASON_NONE);

  // Test opening racks
  cgp_parse_status = game_load_cgp(game, OPENING_CGP);
  assert(cgp_parse_status == CGP_PARSE_STATUS_SUCCESS);
  set_rack_to_string(ld, rack, "ABCDEFG");
  assert(equal_rack(rack, player0_rack));
  set_rack_to_string(ld, rack, "HIJKLM?");
  assert(equal_rack(rack, player1_rack));

  // Test CGP with excessive whitespace
  cgp_parse_status = game_load_cgp(game, EXCESSIVE_WHITESPACE_CGP);
  assert(cgp_parse_status == CGP_PARSE_STATUS_SUCCESS);
  set_rack_to_string(ld, rack, "ABCDEFG");
  assert(equal_rack(rack, player0_rack));
  set_rack_to_string(ld, rack, "HIJKLM?");
  assert(equal_rack(rack, player1_rack));
  assert(game_get_consecutive_scoreless_turns(game) == 4);

  // Test CGP with one consecutive zero
  cgp_parse_status = game_load_cgp(game, ONE_CONSECUTIVE_ZERO_CGP);
  assert(cgp_parse_status == CGP_PARSE_STATUS_SUCCESS);
  assert(game_get_consecutive_scoreless_turns(game) == 1);
  game_reset(game);

  destroy_rack(rack);
  game_destroy(game);
  config_destroy(config);
}

void test_game() {
  test_game_main();
  test_load_cgp();
}