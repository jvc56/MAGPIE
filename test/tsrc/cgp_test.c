#include <assert.h>

#include "../../src/def/validated_move_defs.h"

#include "../../src/ent/game.h"
#include "../../src/ent/validated_move.h"
#include "../../src/impl/config.h"

#include "../../src/impl/cgp.h"
#include "../../src/impl/gameplay.h"

#include "test_constants.h"
#include "test_util.h"

void assert_game_matches_cgp_with_options(const Config *config,
                                          const Game *game,
                                          const char *expected_cgp_with_options,
                                          bool write_player_on_turn_first) {
  PlayersData *players_data = config_get_players_data(config);
  int bingo_bonus = config_get_bingo_bonus(config);
  const char *board_layout_name =
      board_layout_get_name(config_get_board_layout(config));
  const char *ld_name = ld_get_name(config_get_ld(config));
  game_variant_t game_variant = config_get_game_variant(config);

  char *actual_cgp = game_get_cgp_with_options(
      game, write_player_on_turn_first, players_data, bingo_bonus,
      board_layout_name, ld_name, game_variant);

  StringSplitter *split_cgp =
      split_string_by_whitespace(expected_cgp_with_options, true);
  char *expected_cgp_without_options = string_splitter_join(
      split_cgp, 0, string_splitter_get_number_of_items(split_cgp), " ");
  string_splitter_destroy(split_cgp);
  assert_strings_equal(actual_cgp, expected_cgp_without_options);
  free(actual_cgp);
  free(expected_cgp_without_options);
}

void assert_cgp_load_and_write_are_equal(Game *game, const char *load_cgp) {
  game_load_cgp(game, load_cgp);
  assert_game_matches_cgp(game, load_cgp, true);
}

void play_move_and_validate_cgp(Game *game, const char *move_string,
                                const char *rack_string,
                                const char *expected_cgp,
                                bool write_player_on_turn_first) {
  ValidatedMoves *vms =
      validated_moves_create(game, game_get_player_on_turn_index(game),
                             move_string, false, false, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  const Move *move = validated_moves_get_move(vms, 0);
  int player_on_turn_index = game_get_player_on_turn_index(game);
  play_move(move, game, NULL);

  // Return the random rack to the bag and then draw
  // the rack specified by the caller.
  return_rack_to_bag(game, player_on_turn_index);
  draw_rack_string_from_bag(game, player_on_turn_index, rack_string);

  assert_game_matches_cgp(game, expected_cgp, write_player_on_turn_first);
  validated_moves_destroy(vms);
}

void test_cgp_english(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);

  assert_cgp_load_and_write_are_equal(game, EMPTY_CGP);
  assert_cgp_load_and_write_are_equal(game, EMPTY_PLAYER0_RACK_CGP);
  assert_cgp_load_and_write_are_equal(game, EMPTY_PLAYER1_RACK_CGP);
  assert_cgp_load_and_write_are_equal(game, OPENING_CGP);
  assert_cgp_load_and_write_are_equal(game, ONE_CONSECUTIVE_ZERO_CGP);
  assert_cgp_load_and_write_are_equal(game, EXCESSIVE_WHITESPACE_CGP);
  assert_cgp_load_and_write_are_equal(game, DOUG_V_EMELY_DOUBLE_CHALLENGE_CGP);
  assert_cgp_load_and_write_are_equal(game, DOUG_V_EMELY_CGP);
  assert_cgp_load_and_write_are_equal(game, GUY_VS_BOT_ALMOST_COMPLETE_CGP);
  assert_cgp_load_and_write_are_equal(game, GUY_VS_BOT_CGP);
  assert_cgp_load_and_write_are_equal(game, INCOMPLETE_3_CGP);
  assert_cgp_load_and_write_are_equal(game, INCOMPLETE4_CGP);
  assert_cgp_load_and_write_are_equal(game, INCOMPLETE_ELISE_CGP);
  assert_cgp_load_and_write_are_equal(game, INCOMPLETE_CGP);
  assert_cgp_load_and_write_are_equal(game, JOSH2_CGP);
  assert_cgp_load_and_write_are_equal(game, NAME_ISO8859_1_CGP);
  assert_cgp_load_and_write_are_equal(game, NAME_UTF8_NOHEADER_CGP);
  assert_cgp_load_and_write_are_equal(game, NAME_UTF8_WITH_HEADER_CGP);
  assert_cgp_load_and_write_are_equal(game, NOAH_VS_MISHU_CGP);
  assert_cgp_load_and_write_are_equal(game, NOAH_VS_PETER_CGP);
  assert_cgp_load_and_write_are_equal(game, SOME_ISC_GAME_CGP);
  assert_cgp_load_and_write_are_equal(game, UTF8_DOS_CGP);
  assert_cgp_load_and_write_are_equal(game, VS_ANDY_CGP);
  assert_cgp_load_and_write_are_equal(game, VS_FRENTZ_CGP);
  assert_cgp_load_and_write_are_equal(game, VS_ED);
  assert_cgp_load_and_write_are_equal(game, VS_JEREMY);
  assert_cgp_load_and_write_are_equal(game, VS_JEREMY_WITH_P2_RACK);
  assert_cgp_load_and_write_are_equal(game, VS_MATT);
  assert_cgp_load_and_write_are_equal(game, VS_MATT2);
  assert_cgp_load_and_write_are_equal(game, VS_OXY);
  assert_cgp_load_and_write_are_equal(game, TEST_DUPE);
  assert_cgp_load_and_write_are_equal(game, MANY_MOVES);
  assert_cgp_load_and_write_are_equal(game, KA_OPENING_CGP);
  assert_cgp_load_and_write_are_equal(game, AA_OPENING_CGP);
  assert_cgp_load_and_write_are_equal(game, TRIPLE_LETTERS_CGP);
  assert_cgp_load_and_write_are_equal(game, TRIPLE_DOUBLE_CGP);
  assert_cgp_load_and_write_are_equal(game, BOTTOM_LEFT_RE_CGP);
  assert_cgp_load_and_write_are_equal(game, LATER_BETWEEN_DOUBLE_WORDS_CGP);
  assert_cgp_load_and_write_are_equal(game, ION_OPENING_CGP);
  assert_cgp_load_and_write_are_equal(game, ZILLION_OPENING_CGP);
  assert_cgp_load_and_write_are_equal(game, ENTASIS_OPENING_CGP);
  assert_cgp_load_and_write_are_equal(game, UEY_CGP);
  assert_cgp_load_and_write_are_equal(game, OOPSYCHOLOGY_CGP);
  assert_cgp_load_and_write_are_equal(game, DELDAR_VS_HARSHAN_CGP);
  assert_cgp_load_and_write_are_equal(game, FRAWZEY_CGP);
  assert_cgp_load_and_write_are_equal(game, THERMOS_CGP);

  game_reset(game);

  play_move_and_validate_cgp(
      game, "pass", "AAENRSZ",
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AAENRSZ/ 0/0 1", false);
  play_move_and_validate_cgp(
      game, "pass", "DIIIILU",
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AAENRSZ/DIIIILU 0/0 2",
      false);
  play_move_and_validate_cgp(
      game, "pass", "AAENRSZ",
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AAENRSZ/DIIIILU 0/0 3",
      false);
  play_move_and_validate_cgp(
      game, "ex.IIILU", "CDEIRW?",
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AAENRSZ/CDEIRW? 0/0 4",
      false);
  play_move_and_validate_cgp(
      game, "8G.ZA", "AENNRST",
      "15/15/15/15/15/15/15/6ZA7/15/15/15/15/15/15/15 AENNRST/CDEIRW? 22/0 0",
      false);
  play_move_and_validate_cgp(game, "9E.CRoWDIE", "AGNOTT?",
                             "15/15/15/15/15/15/15/6ZA7/4CRoWDIE4/15/15/15/15/"
                             "15/15 AENNRST/AGNOTT? 22/79 0",
                             false);

  game_reset(game);

  play_move_and_validate_cgp(
      game, "pass", "AAENRSZ",
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 /AAENRSZ 0/0 1", true);
  play_move_and_validate_cgp(
      game, "pass", "DIIIILU",
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AAENRSZ/DIIIILU 0/0 2",
      true);
  play_move_and_validate_cgp(
      game, "pass", "AAENRSZ",
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 DIIIILU/AAENRSZ 0/0 3",
      true);
  play_move_and_validate_cgp(
      game, "ex.IIILU", "CDEIRW?",
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AAENRSZ/CDEIRW? 0/0 4",
      true);
  play_move_and_validate_cgp(
      game, "8G.ZA", "AENNRST",
      "15/15/15/15/15/15/15/6ZA7/15/15/15/15/15/15/15 CDEIRW?/AENNRST 0/22 0",
      true);
  play_move_and_validate_cgp(game, "9E.CRoWDIE", "AGNOTT?",
                             "15/15/15/15/15/15/15/6ZA7/4CRoWDIE4/15/15/15/15/"
                             "15/15 AENNRST/AGNOTT? 22/79 0",
                             true);

  game_destroy(game);
  config_destroy(config);
}

void test_cgp_english_with_options(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);

  assert_game_matches_cgp_with_options(
      config, game, EMPTY_CGP_WITHOUT_OPTIONS " lex CSW21;", true);

  load_and_exec_config_or_die(config, "set -bb 27");
  assert_game_matches_cgp_with_options(
      config, game, EMPTY_CGP_WITHOUT_OPTIONS " lex CSW21; bb 27;", true);

  load_and_exec_config_or_die(config, "set -bdn single_row_15");
  assert_game_matches_cgp_with_options(
      config, game,
      EMPTY_CGP_WITHOUT_OPTIONS " lex CSW21; bb 27; bdn single_row_15;", true);

  load_and_exec_config_or_die(config, "set -var wordsmog");
  assert_game_matches_cgp_with_options(
      config, game,
      EMPTY_CGP_WITHOUT_OPTIONS
      " lex CSW21; bb 27; bdn single_row_15; var wordsmog;",
      true);

  load_and_exec_config_or_die(config, "set -l1 NWL20 -l2 CSW21");
  assert_game_matches_cgp_with_options(
      config, game,
      EMPTY_CGP_WITHOUT_OPTIONS " l1 NWL20; l2 CSW21; bb 27; bdn "
                                "single_row_15; ld english; var wordsmog;",
      true);

  game_destroy(game);
  config_destroy(config);
}

void test_cgp_catalan(void) {
  Config *config = config_create_or_die(
      "set -lex DISC2 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);

  assert_cgp_load_and_write_are_equal(game, EMPTY_CATALAN_CGP);
  assert_cgp_load_and_write_are_equal(game, CATALAN_CGP);

  game_destroy(game);
  config_destroy(config);
}

void test_cgp_polish(void) {
  Config *config = config_create_or_die(
      "set -lex OSPS49 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);

  assert_cgp_load_and_write_are_equal(game, POLISH_CGP);
  assert_cgp_load_and_write_are_equal(game, EMPTY_POLISH_CGP);

  game_destroy(game);
  config_destroy(config);
}

void test_cgp(void) {
  test_cgp_english();
  test_cgp_english_with_options();
  test_cgp_catalan();
  test_cgp_polish();
}
