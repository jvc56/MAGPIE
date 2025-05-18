#include <assert.h>

#include "../../src/def/game_defs.h"

#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/player.h"
#include "../../src/ent/rack.h"
#include "../../src/impl/config.h"

#include "../../src/impl/cgp.h"
#include "../../src/util/log.h"

#include "game_test.h"
#include "test_constants.h"
#include "test_util.h"

void reset_and_load_game_failure(Game *game, const char *cgp,
                                 error_code_t expected_cgp_parse_status) {
  ErrorStack *error_stack = error_stack_create();
  game_load_cgp(game, cgp, error_stack);
  assert(error_stack_top(error_stack) == expected_cgp_parse_status);
  error_stack_destroy(error_stack);
}

void test_load_cgp(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  ErrorStack *error_stack = error_stack_create();
  // Test that loading various CGPs doesn't result in
  // any errors
  load_cgp_or_die(game, EMPTY_CGP);
  load_cgp_or_die(game, EMPTY_PLAYER0_RACK_CGP);
  load_cgp_or_die(game, EMPTY_PLAYER1_RACK_CGP);
  load_cgp_or_die(game, OPENING_CGP);
  load_cgp_or_die(game, DOUG_V_EMELY_DOUBLE_CHALLENGE_CGP);
  load_cgp_or_die(game, DOUG_V_EMELY_CGP);
  load_cgp_or_die(game, GUY_VS_BOT_ALMOST_COMPLETE_CGP);
  load_cgp_or_die(game, GUY_VS_BOT_CGP);
  load_cgp_or_die(game, INCOMPLETE_3_CGP);
  load_cgp_or_die(game, INCOMPLETE4_CGP);
  load_cgp_or_die(game, INCOMPLETE_ELISE_CGP);
  load_cgp_or_die(game, INCOMPLETE_CGP);
  load_cgp_or_die(game, JOSH2_CGP);
  load_cgp_or_die(game, NAME_ISO8859_1_CGP);
  load_cgp_or_die(game, NAME_UTF8_NOHEADER_CGP);
  load_cgp_or_die(game, NAME_UTF8_WITH_HEADER_CGP);
  load_cgp_or_die(game, NOAH_VS_MISHU_CGP);
  load_cgp_or_die(game, NOAH_VS_PETER_CGP);
  load_cgp_or_die(game, SOME_ISC_GAME_CGP);
  load_cgp_or_die(game, UTF8_DOS_CGP);
  load_cgp_or_die(game, VS_ANDY_CGP);
  load_cgp_or_die(game, VS_FRENTZ_CGP);

  // Empty string
  reset_and_load_game_failure(game, "",
                              ERROR_STATUS_CGP_PARSE_MISSING_REQUIRED_FIELDS);
  reset_and_load_game_failure(game, "           ",
                              ERROR_STATUS_CGP_PARSE_MISSING_REQUIRED_FIELDS);
  reset_and_load_game_failure(game, "\n\r\t\v\f  ",
                              ERROR_STATUS_CGP_PARSE_MISSING_REQUIRED_FIELDS);
  // Missing board
  reset_and_load_game_failure(game, "/ 0/0 0",
                              ERROR_STATUS_CGP_PARSE_MISSING_REQUIRED_FIELDS);
  // Missing racks
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 0/0 0",
      ERROR_STATUS_CGP_PARSE_MISSING_REQUIRED_FIELDS);
  // Missing scores
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0",
      ERROR_STATUS_CGP_PARSE_MISSING_REQUIRED_FIELDS);

  // Missing consecutive zeros
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0",
      ERROR_STATUS_CGP_PARSE_MISSING_REQUIRED_FIELDS);

  // Too many board rows
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 / 0/0 0",
      ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_BOARD_ROWS);

  // Not enough columns
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/AB3E4F3/15/15/15/15/15/15 / 0/0 0",
      ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_BOARD_COLUMNS);

  // Not enough columns
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/14 / 0/0 0",
      ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_BOARD_COLUMNS);

  // Too many columns
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/AB3CDE4F3/15/15/15/15/15/15 / 0/0 0",
      ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_BOARD_COLUMNS);

  // Too many columns
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/16 / 0/0 0",
      ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_BOARD_COLUMNS);

  // Too many racks
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC/DEF/GHI 0/0 0",
      ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_PLAYER_RACKS);

  // Too few racks
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC 0/0 0",
      ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_PLAYER_RACKS);

  // Too many scores
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC/DEF 123/456/78 0",
      ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_PLAYER_SCORES);

  // Too few scores
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABC/DEF 9000 0",
      ERROR_STATUS_CGP_PARSE_INVALID_NUMBER_OF_PLAYER_SCORES);

  // Invalid board letters
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/ABCD.EFG7/15/15/15/15/15/15 ABC/DEF 0/0 0",
      ERROR_STATUS_CGP_PARSE_MALFORMED_BOARD_LETTERS);

  // Invalid rack letters
  reset_and_load_game_failure(
      game,
      "15/15/15/15/15/15/15/15/3ABCDEFG5/15/15/15/15/15/15 ABC5DF/YXZ 0/0 0",
      ERROR_STATUS_CGP_PARSE_MALFORMED_RACK_LETTERS);

  reset_and_load_game_failure(
      game,
      "15/15/15/15/15/15/15/15/3ABCDEFG5/15/15/15/15/15/15 ABCDF/YX;Z 0/0 0",
      ERROR_STATUS_CGP_PARSE_MALFORMED_RACK_LETTERS);

  // Invalid scores
  reset_and_load_game_failure(game,
                              "15/15/15/15/15/15/15/15/5ABCDEFG3/15/15/15/15/"
                              "15/15 ABCDF/YXZ 234R3/34 0",
                              ERROR_STATUS_CGP_PARSE_MALFORMED_SCORES);

  // Invalid consecutive zeros
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 ABCDF/YXZ 0/0 H",
      ERROR_STATUS_CGP_PARSE_MALFORMED_CONSECUTIVE_ZEROS);

  // Board letters not in bag
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/6ZZZ6/15/15/15/15/15/15 / 0/0 0",
      ERROR_STATUS_CGP_PARSE_BOARD_LETTERS_NOT_IN_BAG);

  // Rack letters not in bag
  reset_and_load_game_failure(
      game, "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 Z/ZZ 0/0 0",
      ERROR_STATUS_CGP_PARSE_RACK_LETTERS_NOT_IN_BAG);

  load_and_exec_config_or_die(config, "cgp " VS_FRENTZ_CGP);

  config_load_command(
      config, "cgp 15/15/15/15/15/15/15/15/6ZZZ6/15/15/15/15/15/15 / 0/0 0",
      error_stack);
  assert(error_stack_is_empty(error_stack));
  config_execute_command(config, error_stack);
  error_stack_print(error_stack);
  assert(error_stack_top(error_stack) ==
         ERROR_STATUS_CGP_PARSE_BOARD_LETTERS_NOT_IN_BAG);

  assert_game_matches_cgp(config_get_game(config), VS_FRENTZ_CGP, true);

  error_stack_destroy(error_stack);
  game_destroy(game);
  config_destroy(config);
}

void test_game_main(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  Rack *rack = rack_create(ld_get_size(ld));

  Rack *player0_rack = player_get_rack(game_get_player(game, 0));
  Rack *player1_rack = player_get_rack(game_get_player(game, 1));

  // Test Reset
  game_set_consecutive_scoreless_turns(game, 3);
  game_set_game_end_reason(game, GAME_END_REASON_STANDARD);
  game_reset(game);
  assert(game_get_consecutive_scoreless_turns(game) == 0);
  assert(!game_over(game));

  // Test opening racks
  load_cgp_or_die(game, OPENING_CGP);
  rack_set_to_string(ld, rack, "ABCDEFG");
  assert(equal_rack(rack, player0_rack));
  rack_set_to_string(ld, rack, "HIJKLM?");
  assert(equal_rack(rack, player1_rack));

  // Test CGP with excessive whitespace
  load_cgp_or_die(game, EXCESSIVE_WHITESPACE_CGP);
  rack_set_to_string(ld, rack, "ABCDEFG");
  assert(equal_rack(rack, player0_rack));
  rack_set_to_string(ld, rack, "HIJKLM?");
  assert(equal_rack(rack, player1_rack));
  assert(game_get_consecutive_scoreless_turns(game) == 4);

  // Test CGP with one consecutive zero
  load_cgp_or_die(game, ONE_CONSECUTIVE_ZERO_CGP);
  assert(game_get_consecutive_scoreless_turns(game) == 1);
  game_reset(game);

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void test_game(void) {
  test_game_main();
  test_load_cgp();
}