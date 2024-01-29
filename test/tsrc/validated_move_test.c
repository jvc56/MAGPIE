#include <assert.h>

#include "../../src/def/letter_distribution_defs.h"
#include "../../src/def/validated_move_defs.h"

#include "../../src/ent/config.h"
#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/rack.h"
#include "../../src/ent/validated_move.h"

#include "../../src/str/move_string.h"

#include "test_constants.h"
#include "test_util.h"

void assert_validated_move_error(
    Game *game, const char *cgp_str, const char *move_str, int player_index,
    bool allow_phonies, move_validation_status_t expected_error_status) {
  load_cgp_or_die(game, cgp_str);
  ValidatedMoves *vms =
      validated_moves_create(game, player_index, move_str, allow_phonies);
  move_validation_status_t actual_error_status =
      validated_moves_get_validation_status(vms);
  assert(actual_error_status == expected_error_status);
  validated_moves_destroy(vms);
}

ValidatedMoves *assert_validated_move_success(Game *game, const char *cgp_str,
                                              const char *move_str,
                                              int player_index,
                                              bool allow_phonies) {
  load_cgp_or_die(game, cgp_str);
  ValidatedMoves *vms =
      validated_moves_create(game, player_index, move_str, allow_phonies);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  return vms;
}

void test_validated_move_errors() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);

  assert_validated_move_error(game, EMPTY_CGP, NULL, 0, false,
                              MOVE_VALIDATION_STATUS_EMPTY_MOVE);
  assert_validated_move_error(game, EMPTY_CGP, "", 0, false,
                              MOVE_VALIDATION_STATUS_EMPTY_MOVE);
  assert_validated_move_error(game, EMPTY_CGP, "          \n  ", 0, false,
                              MOVE_VALIDATION_STATUS_EMPTY_MOVE);
  assert_validated_move_error(game, EMPTY_CGP, "ex.ABC", 2, false,
                              MOVE_VALIDATION_STATUS_INVALID_PLAYER_INDEX);
  assert_validated_move_error(game, EMPTY_CGP, "ex.ABC", -1, false,
                              MOVE_VALIDATION_STATUS_INVALID_PLAYER_INDEX);
  assert_validated_move_error(
      game, EMPTY_CGP, ".ABC", 0, false,
      MOVE_VALIDATION_STATUS_EMPTY_MOVE_TYPE_OR_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "  .ABC.ABCDEF", 0, false,
      MOVE_VALIDATION_STATUS_EMPTY_MOVE_TYPE_OR_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "  \n  .HADJI.ADHIJ.0.0", 0, false,
      MOVE_VALIDATION_STATUS_EMPTY_MOVE_TYPE_OR_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "      .4", 0, false,
      MOVE_VALIDATION_STATUS_EMPTY_MOVE_TYPE_OR_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "ex.", 0, false,
      MOVE_VALIDATION_STATUS_EMPTY_TILES_PLAYED_OR_NUMBER_EXCHANGED);
  assert_validated_move_error(
      game, EMPTY_CGP, "ex.  .ABCDEF", 0, false,
      MOVE_VALIDATION_STATUS_EMPTY_TILES_PLAYED_OR_NUMBER_EXCHANGED);
  assert_validated_move_error(
      game, EMPTY_CGP, "h7..ADHIJ.0.0", 0, false,
      MOVE_VALIDATION_STATUS_EMPTY_TILES_PLAYED_OR_NUMBER_EXCHANGED);
  assert_validated_move_error(game, EMPTY_CGP, "ex.ABC.", 0, false,
                              MOVE_VALIDATION_STATUS_EMPTY_RACK);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.   .0.0", 0, false,
                              MOVE_VALIDATION_STATUS_EMPTY_RACK);
  assert_validated_move_error(game, EMPTY_CGP, "ex.4. ", 0, false,
                              MOVE_VALIDATION_STATUS_EMPTY_RACK);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADHIJ..0", 0, false,
                              MOVE_VALIDATION_STATUS_EMPTY_CHALLENGE_POINTS);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADHIJ.0.  ", 0, false,
                              MOVE_VALIDATION_STATUS_EMPTY_CHALLENGE_TURN_LOSS);
  assert_validated_move_error(game, EMPTY_CGP, "h8.12345", 0, false,
                              MOVE_VALIDATION_STATUS_NONEXCHANGE_NUMERIC_TILES);
  assert_validated_move_error(game, EMPTY_CGP, "ex.8", 0, false,
                              MOVE_VALIDATION_STATUS_INVALID_NUMBER_EXCHANGED);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HA#DJI", 0, false,
                              MOVE_VALIDATION_STATUS_INVALID_TILES_PLAYED);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADH3JI ", 0, false,
                              MOVE_VALIDATION_STATUS_INVALID_RACK);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADHIJ.-1.0", 0, false,
                              MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_POINTS);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADHIJ.h.0", 0, false,
                              MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_POINTS);
  assert_validated_move_error(
      game, EMPTY_CGP, "h8.HADJI.ADHIJ.0.-1", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_TURN_LOSS);
  assert_validated_move_error(
      game, EMPTY_CGP, "h8.HADJI.ADHIJ.0.2", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_TURN_LOSS);
  assert_validated_move_error(
      game, EMPTY_CGP, "h8.HADJI.ADHIJ.0.B", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_TURN_LOSS);
  assert_validated_move_error(game, EMPTY_CGP, "8H.PIZAZZ.AIPZZZ", 0, false,
                              MOVE_VALIDATION_STATUS_RACK_NOT_IN_BAG);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.BHAJI", 0, false,
                              MOVE_VALIDATION_STATUS_TILES_PLAYED_NOT_IN_RACK);
  assert_validated_move_error(game, ION_OPENING_CGP, "h1.AERATIONS", 0, false,
                              MOVE_VALIDATION_STATUS_TILES_PLAYED_OVERFLOW);
  assert_validated_move_error(
      game, ION_OPENING_CGP, "h8.QAT", 0, false,
      MOVE_VALIDATION_STATUS_TILES_PLAYED_BOARD_MISMATCH);
  assert_validated_move_error(game, EMPTY_CGP, "1A.QAT", 0, false,
                              MOVE_VALIDATION_STATUS_TILES_PLAYED_DISCONNECTED);
  assert_validated_move_error(game, ION_OPENING_CGP, "1A.QAT", 0, false,
                              MOVE_VALIDATION_STATUS_TILES_PLAYED_DISCONNECTED);
  assert_validated_move_error(
      game, EMPTY_CGP, "*.QAT", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "8.QAT", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H.QAT", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "8H1.QAT", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H16.QAT", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "8P.QAT", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "P8.QAT", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H8H.QAT", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H0H.QAT", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "0H0.QAT", 0, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(game, EMPTY_CGP, "ex.ABC.ABCDEF.4", 0, false,
                              MOVE_VALIDATION_STATUS_EXCESS_EXCHANGE_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "ex.ABC.ABCDEF.3.0", 0, false,
                              MOVE_VALIDATION_STATUS_EXCESS_EXCHANGE_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADHIJ.0.0.1", 0, false,
                              MOVE_VALIDATION_STATUS_EXCESS_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "ex", 0, false,
                              MOVE_VALIDATION_STATUS_MISSING_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "h8", 0, false,
                              MOVE_VALIDATION_STATUS_MISSING_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "pass.ABC", 0, false,
                              MOVE_VALIDATION_STATUS_EXCESS_PASS_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "h8.WECH", 0, false,
                              MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
  // Forms AION*
  assert_validated_move_error(game, ION_OPENING_CGP, "E5.RETAILS", 0, false,
                              MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
  // Forms 7 valid words and 1 phony word:
  // valid: REALISE, RE, EN, AT, LA, IS, SI
  // phony: TS*
  assert_validated_move_error(game, ENTASIS_OPENING_CGP, "7C.REALIST", 0, false,
                              MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
  game_destroy(game);
  config_destroy(config);
}

void test_validated_move_success() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  ValidatedMoves *vms = NULL;
  Move *move = NULL;
  Rack *rack = rack_create(ld_get_size(ld));

  vms = assert_validated_move_success(game, EMPTY_CGP, "pass", 0, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_PASS);
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(game, EMPTY_CGP, "ex.ABC", 0, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(!validated_moves_get_unknown_exchange(vms, 0));
  assert(move_get_type(move) == GAME_EVENT_EXCHANGE);
  assert(move_get_tiles_length(move) == 3);
  assert(move_get_tiles_played(move) == 3);
  assert(move_get_score(move) == 0);
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "B"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "C"));
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(game, EMPTY_CGP, "ex.4", 0, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(validated_moves_get_unknown_exchange(vms, 0));
  assert(move_get_type(move) == GAME_EVENT_EXCHANGE);
  assert(move_get_tiles_length(move) == 4);
  assert(move_get_tiles_played(move) == 4);
  assert(move_get_score(move) == 0);
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  rack_set_to_string(ld, rack, "ABCDEFG");
  vms = assert_validated_move_success(game, EMPTY_CGP, "ex.ABC.ABCDEFG", 0,
                                      false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(!validated_moves_get_unknown_exchange(vms, 0));
  assert(move_get_type(move) == GAME_EVENT_EXCHANGE);
  assert(move_get_tiles_length(move) == 3);
  assert(move_get_tiles_played(move) == 3);
  assert(move_get_score(move) == 0);
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "B"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "C"));
  assert(racks_are_equal(validated_moves_get_rack(vms, 0), rack));
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  rack_set_to_string(ld, rack, "AAIORT?");
  vms = assert_validated_move_success(game, ION_OPENING_CGP,
                                      "H1.AeRATION.AAIORT?.3.1", 0, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 8);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_row_start(move) == 0);
  assert(move_get_col_start(move) == 7);
  assert(move_get_dir(move) == BOARD_VERTICAL_DIRECTION);
  assert(move_get_score(move) == 74);
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "e"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "R"));
  assert(move_get_tile(move, 3) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 4) == ld_hl_to_ml(ld, "T"));
  assert(move_get_tile(move, 5) == ld_hl_to_ml(ld, "I"));
  assert(move_get_tile(move, 6) == ld_hl_to_ml(ld, "O"));
  assert(move_get_tile(move, 7) == PLAYED_THROUGH_MARKER);
  assert(racks_are_equal(validated_moves_get_rack(vms, 0), rack));
  assert(validated_moves_get_challenge_points(vms, 0) == 3);
  assert(validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  // horizontal, full rack unknown. No challenge pts, no challenge turn loss
  // h4.HADJI.ADHIJ.0.0

  // same as above
  // h4.HADJI

  // vertical play, through a letter;
  // letter will need to be determined by the history
  // firefang was wrongly challenged and so the opponent loses their turn
  // 11d.FIREFANG.AEFGINR.0.1

  // chthonic was played with a blank O, through
  // some letter. It gets an extra 5-pt bonus.
  // 3m.CHTHoNIC.CCHIN?T.5.0

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void test_validated_move() {
  // Test:
  // all success cases in validate_split_move
  // player index kwg
  // multiple moves
  // combining moves
  // FIXME: check opening square validation
  test_validated_move_errors();
  test_validated_move_success();
}
