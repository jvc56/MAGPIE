#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/config.h"
#include "../src/game.h"
#include "game_test.h"
#include "rack_test.h"
#include "test_constants.h"
#include "test_util.h"

void reset_and_load_game_success(Game *game, const char *cgp) {
  reset_game(game);
  cgp_parse_status_t cgp_parse_status = load_cgp(game, cgp);
  assert(cgp_parse_status == CGP_PARSE_STATUS_SUCCESS);
}

void reset_and_load_game_failure(Game *game, const char *cgp,
                                 cgp_parse_status_t expected_cgp_parse_status) {
  reset_game(game);
  cgp_parse_status_t cgp_parse_status = load_cgp(game, cgp);
  assert(cgp_parse_status == expected_cgp_parse_status);
}

void test_load_cgp(TestConfig *testconfig) {
  Config *config = get_nwl_config(testconfig);
  Game *game = create_game(config);
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

  destroy_game(game);
}

void test_game_main(TestConfig *testconfig) {
  Config *config = get_nwl_config(testconfig);
  Game *game = create_game(config);
  Rack *rack = create_rack(config->letter_distribution->size);
  cgp_parse_status_t cgp_parse_status;

  // Test Reset
  game->consecutive_scoreless_turns = 3;
  game->game_end_reason = GAME_END_REASON_STANDARD;
  reset_game(game);
  assert(game->consecutive_scoreless_turns == 0);
  assert(game->game_end_reason == GAME_END_REASON_NONE);

  // Test opening racks
  cgp_parse_status = load_cgp(game, OPENING_CGP);
  assert(cgp_parse_status == CGP_PARSE_STATUS_SUCCESS);
  set_rack_to_string(rack, "ABCDEFG", config->letter_distribution);
  assert(equal_rack(rack, game->players[0]->rack));
  set_rack_to_string(rack, "HIJKLM?", config->letter_distribution);
  assert(equal_rack(rack, game->players[1]->rack));
  reset_game(game);

  // Test CGP with excessive whitespace
  cgp_parse_status = load_cgp(game, EXCESSIVE_WHITESPACE_CGP);
  assert(cgp_parse_status == CGP_PARSE_STATUS_SUCCESS);
  set_rack_to_string(rack, "ABCDEFG", config->letter_distribution);
  assert(equal_rack(rack, game->players[0]->rack));
  set_rack_to_string(rack, "HIJKLM?", config->letter_distribution);
  assert(equal_rack(rack, game->players[1]->rack));
  assert(game->consecutive_scoreless_turns == 4);
  reset_game(game);

  // Test CGP with one consecutive zero
  cgp_parse_status = load_cgp(game, ONE_CONSECUTIVE_ZERO_CGP);
  assert(cgp_parse_status == CGP_PARSE_STATUS_SUCCESS);
  assert(game->consecutive_scoreless_turns == 1);
  reset_game(game);

  destroy_rack(rack);
  destroy_game(game);
}

void test_load_cgp_operations() {
  CGPOperations *cgp_operations = get_default_cgp_operations();
  cgp_parse_status_t cgp_parse_status;

  const char *cgp_success =
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 bb 33 bdn "
      "SuperCrosswordGame var wordsmog ld english lex "
      "CSW21;";
  cgp_parse_status = load_cgp_operations(cgp_operations, cgp_success);
  assert(cgp_parse_status == CGP_PARSE_STATUS_SUCCESS);
  assert(cgp_operations->bingo_bonus == 33);
  assert(cgp_operations->board_layout == BOARD_LAYOUT_SUPER_CROSSWORD_GAME);
  assert(cgp_operations->game_variant == GAME_VARIANT_WORDSMOG);
  assert_strings_equal(cgp_operations->lexicon_name, "CSW21");
  assert_strings_equal(cgp_operations->letter_distribution_name, "english");

  const char *cgp_malformed_bingo_bonus =
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 bb 3r3 bdn "
      "SuperCrosswordGame var wordsmog ld english lex "
      "CSW21;";
  cgp_parse_status =
      load_cgp_operations(cgp_operations, cgp_malformed_bingo_bonus);
  assert(cgp_parse_status == CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_BINGO_BONUS);

  const char *cgp_malformed_board_name =
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 bb 33 bdn "
      "MiniCrosswordGame var wordsmog ld english lex "
      "CSW21;";
  cgp_parse_status =
      load_cgp_operations(cgp_operations, cgp_malformed_board_name);
  assert(cgp_parse_status == CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_BOARD_NAME);

  const char *cgp_malformed_game_variant =
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0 bb 33 bdn "
      "SuperCrosswordGame var ifonly ld english lex "
      "CSW21;";
  cgp_parse_status =
      load_cgp_operations(cgp_operations, cgp_malformed_game_variant);
  assert(cgp_parse_status ==
         CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_GAME_VARIANT);

  destroy_cgp_operations(cgp_operations);
}

void test_game(TestConfig *testconfig) {
  test_game_main(testconfig);
  test_load_cgp(testconfig);
  test_load_cgp_operations();
}