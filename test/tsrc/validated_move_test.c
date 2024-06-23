#include <assert.h>

#include "../../src/def/letter_distribution_defs.h"
#include "../../src/def/validated_move_defs.h"

#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/rack.h"
#include "../../src/ent/validated_move.h"
#include "../../src/impl/config.h"

#include "../../src/impl/gameplay.h"

#include "../../src/str/move_string.h"

#include "test_constants.h"
#include "test_util.h"

void assert_validated_move_error(
    Game *game, const char *cgp_str, const char *move_str, int player_index,
    bool allow_phonies, bool allow_unknown_exchanges, bool allow_playthrough,
    move_validation_status_t expected_error_status) {
  load_cgp_or_die(game, cgp_str);
  ValidatedMoves *vms =
      validated_moves_create(game, player_index, move_str, allow_phonies,
                             allow_unknown_exchanges, allow_playthrough);
  move_validation_status_t actual_error_status =
      validated_moves_get_validation_status(vms);
  assert(actual_error_status == expected_error_status);
  validated_moves_destroy(vms);
}

void test_validated_move_errors(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);

  assert_validated_move_error(game, EMPTY_CGP, "", 0, false, true, false,
                              MOVE_VALIDATION_STATUS_EMPTY_MOVE);
  assert_validated_move_error(game, EMPTY_CGP, "          \n  ", 0, false, true,
                              false, MOVE_VALIDATION_STATUS_EMPTY_MOVE);
  assert_validated_move_error(game, EMPTY_CGP, "ex.ABC", 2, false, true, false,
                              MOVE_VALIDATION_STATUS_INVALID_PLAYER_INDEX);
  assert_validated_move_error(game, EMPTY_CGP, "ex.ABC", -1, false, true, false,
                              MOVE_VALIDATION_STATUS_INVALID_PLAYER_INDEX);
  assert_validated_move_error(
      game, EMPTY_CGP, ".ABC", 0, false, true, false,
      MOVE_VALIDATION_STATUS_EMPTY_MOVE_TYPE_OR_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "  .ABC.ABCDEF", 0, false, true, false,
      MOVE_VALIDATION_STATUS_EMPTY_MOVE_TYPE_OR_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "  \n  .HADJI.ADHIJ.0.0", 0, false, true, false,
      MOVE_VALIDATION_STATUS_EMPTY_MOVE_TYPE_OR_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "      .4", 0, false, true, false,
      MOVE_VALIDATION_STATUS_EMPTY_MOVE_TYPE_OR_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "ex.", 0, false, true, false,
      MOVE_VALIDATION_STATUS_EMPTY_TILES_PLAYED_OR_NUMBER_EXCHANGED);
  assert_validated_move_error(
      game, EMPTY_CGP, "ex.  .ABCDEF", 0, false, true, false,
      MOVE_VALIDATION_STATUS_EMPTY_TILES_PLAYED_OR_NUMBER_EXCHANGED);
  assert_validated_move_error(
      game, EMPTY_CGP, "h7..ADHIJ.0.0", 0, false, true, false,
      MOVE_VALIDATION_STATUS_EMPTY_TILES_PLAYED_OR_NUMBER_EXCHANGED);
  assert_validated_move_error(game, EMPTY_CGP, "ex.ABC.", 0, false, true, false,
                              MOVE_VALIDATION_STATUS_EMPTY_RACK);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.   .0.0", 0, false,
                              true, false, MOVE_VALIDATION_STATUS_EMPTY_RACK);
  assert_validated_move_error(game, EMPTY_CGP, "ex.4. ", 0, false, true, false,
                              MOVE_VALIDATION_STATUS_EMPTY_RACK);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADHIJ..0", 0, false,
                              true, false,
                              MOVE_VALIDATION_STATUS_EMPTY_CHALLENGE_POINTS);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADHIJ.0.  ", 0, false,
                              true, false,
                              MOVE_VALIDATION_STATUS_EMPTY_CHALLENGE_TURN_LOSS);
  assert_validated_move_error(game, EMPTY_CGP, "h8.12345", 0, false, true,
                              false,
                              MOVE_VALIDATION_STATUS_NONEXCHANGE_NUMERIC_TILES);
  assert_validated_move_error(game, EMPTY_CGP, "ex.8", 0, false, true, false,
                              MOVE_VALIDATION_STATUS_INVALID_NUMBER_EXCHANGED);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HA#DJI", 0, false, true,
                              false,
                              MOVE_VALIDATION_STATUS_INVALID_TILES_PLAYED);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HA#DJI", 0, false, true,
                              false,
                              MOVE_VALIDATION_STATUS_INVALID_TILES_PLAYED);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADH3JI ", 0, false,
                              true, false, MOVE_VALIDATION_STATUS_INVALID_RACK);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADHIJ.-1.0", 0, false,
                              false, false,
                              MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_POINTS);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADHIJ.h.0", 0, false,
                              false, false,
                              MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_POINTS);
  assert_validated_move_error(
      game, EMPTY_CGP, "h8.HADJI.ADHIJ.0.-1", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_TURN_LOSS);
  assert_validated_move_error(
      game, EMPTY_CGP, "h8.HADJI.ADHIJ.0.2", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_TURN_LOSS);
  assert_validated_move_error(
      game, EMPTY_CGP, "h8.HADJI.ADHIJ.0.B", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_CHALLENGE_TURN_LOSS);
  assert_validated_move_error(game, EMPTY_CGP, "8H.PIZAZZ.AIPZZZ", 0, false,
                              true, false,
                              MOVE_VALIDATION_STATUS_RACK_NOT_IN_BAG);
  assert_validated_move_error(game, EMPTY_CGP, "8H.BIBB.ABBBIII", 0, false,
                              true, false,
                              MOVE_VALIDATION_STATUS_RACK_NOT_IN_BAG);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.BHAJI", 0, false, true,
                              false,
                              MOVE_VALIDATION_STATUS_TILES_PLAYED_NOT_IN_RACK);
  assert_validated_move_error(game, ION_OPENING_CGP, "h1.AERATIONS", 0, false,
                              false, false,
                              MOVE_VALIDATION_STATUS_TILES_PLAYED_OVERFLOW);
  assert_validated_move_error(
      game, ION_OPENING_CGP, "8A.AERATINGS", 0, false, false, false,
      MOVE_VALIDATION_STATUS_TILES_PLAYED_BOARD_MISMATCH);
  assert_validated_move_error(
      game, ION_OPENING_CGP, "h8.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_TILES_PLAYED_BOARD_MISMATCH);
  assert_validated_move_error(game, EMPTY_CGP, "1A.QAT", 0, false, true, false,
                              MOVE_VALIDATION_STATUS_TILES_PLAYED_DISCONNECTED);
  assert_validated_move_error(game, ION_OPENING_CGP, "1A.QAT", 0, false, true,
                              false,
                              MOVE_VALIDATION_STATUS_TILES_PLAYED_DISCONNECTED);
  assert_validated_move_error(
      game, EMPTY_CGP, "*.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "8.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "8H1.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H16.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "8P.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "P8.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H8H.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "H0H.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(
      game, EMPTY_CGP, "0H0.QAT", 0, false, true, false,
      MOVE_VALIDATION_STATUS_INVALID_TILE_PLACEMENT_POSITION);
  assert_validated_move_error(game, EMPTY_CGP, "ex.ABC.ABCDEF.4", 0, false,
                              true, false,
                              MOVE_VALIDATION_STATUS_EXCESS_EXCHANGE_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "ex.ABC.ABCDEF.3.0", 0, false,
                              true, false,
                              MOVE_VALIDATION_STATUS_EXCESS_EXCHANGE_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "h8.HADJI.ADHIJ.0.0.1", 0, false,
                              true, false,
                              MOVE_VALIDATION_STATUS_EXCESS_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "ex", 0, false, true, false,
                              MOVE_VALIDATION_STATUS_MISSING_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "h8", 0, false, true, false,
                              MOVE_VALIDATION_STATUS_MISSING_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "pass.ABC.AB", 0, false, true,
                              false, MOVE_VALIDATION_STATUS_EXCESS_PASS_FIELDS);
  assert_validated_move_error(game, EMPTY_CGP, "h8.WECH", 0, false, true, false,
                              MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
  // Forms AION*
  assert_validated_move_error(game, ION_OPENING_CGP, "E5.RETAILS", 0, false,
                              true, false,
                              MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
  // Forms 7 valid words and 1 phony word:
  // valid: REALISE, RE, EN, AT, LA, IS, SI
  // phony: TS*
  assert_validated_move_error(game, ENTASIS_OPENING_CGP, "7C.REALIST", 0, false,
                              true, false,
                              MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);

  assert_validated_move_error(
      game, EMPTY_CGP, "ex.4", 0, false, false, false,
      MOVE_VALIDATION_STATUS_UNKNOWN_EXCHANGE_DISALLOWED);

  assert_validated_move_error(
      game, WORMROOT_CGP, "ex.BFQR.BFQRTTV", 0, false, false, false,
      MOVE_VALIDATION_STATUS_EXCHANGE_INSUFFICIENT_TILES);

  assert_validated_move_error(
      game, WORMROOT_CGP, "ex.4", 0, false, true, false,
      MOVE_VALIDATION_STATUS_EXCHANGE_INSUFFICIENT_TILES);

  game_destroy(game);
  config_destroy(config);
}

void test_validated_move_success(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player0 = game_get_player(game, 0);
  const KLV *player_0_klv = player_get_klv(player0);
  ValidatedMoves *vms = NULL;
  const Move *move = NULL;
  Rack *rack = rack_create(ld_get_size(ld));
  Rack *leave = rack_create(ld_get_size(ld));

  vms = assert_validated_move_success(game, EMPTY_CGP, "pass", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_PASS);
  assert(move_get_score(move) == 0);
  assert(within_epsilon(move_get_equity(move), PASS_MOVE_EQUITY));
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  rack_set_to_string(ld, rack, "ACEGIK");
  rack_set_to_string(ld, leave, "ACEGIK");
  vms = assert_validated_move_success(game, EMPTY_CGP, "pass.ACEGIK", 0, false,
                                      false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_PASS);
  assert(move_get_score(move) == 0);
  assert(within_epsilon(move_get_equity(move), PASS_MOVE_EQUITY));
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  assert(within_epsilon(move_get_equity(move), PASS_MOVE_EQUITY));
  assert(racks_are_equal(validated_moves_get_rack(vms, 0), rack));
  validated_moves_destroy(vms);

  vms =
      assert_validated_move_success(game, EMPTY_CGP, "ex.ABC", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(!validated_moves_get_unknown_exchange(vms, 0));
  assert(move_get_type(move) == GAME_EVENT_EXCHANGE);
  assert(move_get_tiles_length(move) == 3);
  assert(move_get_tiles_played(move) == 3);
  assert(move_get_score(move) == 0);
  // Rack is empty, so equity should be zero
  assert(within_epsilon(move_get_equity(move), 0));
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "B"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "C"));
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(game, EMPTY_CGP, "ex.4", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(validated_moves_get_unknown_exchange(vms, 0));
  assert(move_get_type(move) == GAME_EVENT_EXCHANGE);
  assert(move_get_tiles_length(move) == 4);
  assert(move_get_tiles_played(move) == 4);
  assert(move_get_score(move) == 0);
  assert(within_epsilon(move_get_equity(move), 0));
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  rack_set_to_string(ld, rack, "ABCDEFG");
  vms = assert_validated_move_success(game, EMPTY_CGP, "ex.ABC.ABCDEFG", 0,
                                      false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(!validated_moves_get_unknown_exchange(vms, 0));
  assert(move_get_type(move) == GAME_EVENT_EXCHANGE);
  assert(move_get_tiles_length(move) == 3);
  assert(move_get_tiles_played(move) == 3);
  assert(move_get_score(move) == 0);
  rack_set_to_string(ld, leave, "DEFG");
  assert(within_epsilon(move_get_equity(move),
                        klv_get_leave_value(player_0_klv, leave)));
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "B"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "C"));
  assert(racks_are_equal(validated_moves_get_rack(vms, 0), rack));
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  rack_set_to_string(ld, rack, "AAIORT?");
  vms = assert_validated_move_success(
      game, ION_OPENING_CGP, "H1.AeRATION.AAIORT?.3.1", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 8);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_row_start(move) == 0);
  assert(move_get_col_start(move) == 7);
  assert(move_get_dir(move) == BOARD_VERTICAL_DIRECTION);
  assert(move_get_score(move) == 74);
  assert(within_epsilon(move_get_equity(move), 74));
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

  for (int dir = 0; dir < 2; dir++) {
    if (dir == BOARD_HORIZONTAL_DIRECTION) {
      vms = assert_validated_move_success(game, EMPTY_CGP, "8d.JIHAD", 0, false,
                                          false);
    } else {
      vms = assert_validated_move_success(game, EMPTY_CGP, "H4.JIHAD", 0, false,
                                          false);
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
    assert(move_get_score(move) == 48);
    assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "J"));
    assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "I"));
    assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "H"));
    assert(move_get_tile(move, 3) == ld_hl_to_ml(ld, "A"));
    assert(move_get_tile(move, 4) == ld_hl_to_ml(ld, "D"));
    assert(!validated_moves_get_rack(vms, 0));
    assert(validated_moves_get_challenge_points(vms, 0) == 0);
    assert(!validated_moves_get_challenge_turn_loss(vms, 0));
    validated_moves_destroy(vms);
  }

  rack_set_to_string(ld, rack, "AEFFGIR");
  vms = assert_validated_move_success(
      game, ION_OPENING_CGP, "H2.FIREFANG.AEFFGIR.0.1", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 8);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_row_start(move) == 1);
  assert(move_get_col_start(move) == 7);
  assert(move_get_dir(move) == BOARD_VERTICAL_DIRECTION);
  assert(move_get_score(move) == 66);
  assert(within_epsilon(move_get_equity(move), 66));
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "F"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "I"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "R"));
  assert(move_get_tile(move, 3) == ld_hl_to_ml(ld, "E"));
  assert(move_get_tile(move, 4) == ld_hl_to_ml(ld, "F"));
  assert(move_get_tile(move, 5) == ld_hl_to_ml(ld, "A"));
  assert(move_get_tile(move, 6) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 7) == ld_hl_to_ml(ld, "G"));
  assert(racks_are_equal(validated_moves_get_rack(vms, 0), rack));
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(game, VS_ED, "N11.PeNT", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 4);
  assert(move_get_tiles_played(move) == 4);
  assert(move_get_row_start(move) == 10);
  assert(move_get_col_start(move) == 13);
  assert(move_get_dir(move) == BOARD_VERTICAL_DIRECTION);
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  rack_set_to_string(ld, rack, "DDESW??");
  vms = assert_validated_move_success(
      game, VS_JEREMY, "14B.hEaDWORDS.DDESW??.0.0", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 9);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_row_start(move) == 13);
  assert(move_get_col_start(move) == 1);
  assert(move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION);
  assert(move_get_score(move) == 106);
  assert(move_get_tile(move, 0) == ld_hl_to_ml(ld, "h"));
  assert(move_get_tile(move, 1) == ld_hl_to_ml(ld, "E"));
  assert(move_get_tile(move, 2) == ld_hl_to_ml(ld, "a"));
  assert(move_get_tile(move, 3) == ld_hl_to_ml(ld, "D"));
  assert(move_get_tile(move, 4) == ld_hl_to_ml(ld, "W"));
  assert(move_get_tile(move, 5) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 6) == PLAYED_THROUGH_MARKER);
  assert(move_get_tile(move, 7) == ld_hl_to_ml(ld, "D"));
  assert(move_get_tile(move, 8) == ld_hl_to_ml(ld, "S"));
  assert(racks_are_equal(validated_moves_get_rack(vms, 0), rack));
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  rack_set_to_string(ld, rack, "ABEOPXZ");
  vms = assert_validated_move_success(
      game, VS_OXY, "A1.OXYPHENBUTAZONE.ABEOPXZ.0.0", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_tiles_length(move) == 15);
  assert(move_get_tiles_played(move) == 7);
  assert(move_get_row_start(move) == 0);
  assert(move_get_col_start(move) == 0);
  assert(move_get_dir(move) == BOARD_VERTICAL_DIRECTION);
  assert(move_get_score(move) == 1780);
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
  assert(racks_are_equal(validated_moves_get_rack(vms, 0), rack));
  assert(validated_moves_get_challenge_points(vms, 0) == 0);
  assert(!validated_moves_get_challenge_turn_loss(vms, 0));
  validated_moves_destroy(vms);

  // Test allowing playthrough tiles
  // Any number of play through chars is allowed
  vms = assert_validated_move_success(
      game, VS_OXY, "A1.OX$P$$$B$$AZ$$E.ABEOPXZ.0.0", 0, false, true);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      game, VS_OXY, "A1.OXYPHENBU$AZONE.ABEOPXZ.0.0", 0, false, true);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      game, VS_OXY, "A1.OX$PHENBU$AZONE.ABEOPXZ.0.0", 0, false, true);
  validated_moves_destroy(vms);

  // Form a bunch of phonies which we will allow.
  vms = assert_validated_move_success(game, ENTASIS_OPENING_CGP, "7C.VRRUWIW",
                                      0, true, false);
  validated_moves_destroy(vms);

  // Test equity
  player_set_move_sort_type(player0, MOVE_SORT_EQUITY);
  vms = assert_validated_move_success(game, ION_OPENING_CGP, "9G.NON.NONAIER",
                                      0, false, false);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_score(move) == 10);
  rack_set_to_string(ld, leave, "AEIR");
  assert(within_epsilon(move_get_equity(move),
                        10 + klv_get_leave_value(player_0_klv, leave)));
  validated_moves_destroy(vms);

  player_set_move_sort_type(player0, MOVE_SORT_SCORE);
  vms = assert_validated_move_success(game, ION_OPENING_CGP, "9g.NON.NONAIER",
                                      0, false, false);
  move = validated_moves_get_move(vms, 0);
  assert(move_get_score(move) == 10);
  assert(within_epsilon(move_get_equity(move), 10));
  validated_moves_destroy(vms);

  rack_destroy(rack);
  rack_destroy(leave);
  game_destroy(game);
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

  vms = assert_validated_move_success(game, ION_OPENING_CGP, "7f.IEE", 0, true,
                                      false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  assert(move_get_score(validated_moves_get_move(vms, 0)) == 11);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      game, THERMOS_CGP, "3B.HITHERMOST,3B.NETHERMOST", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 2);
  assert(move_get_score(validated_moves_get_move(vms, 0)) == 36);
  assert(move_get_score(validated_moves_get_move(vms, 1)) == 30);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(game, VS_ED, "5B.AIRGLOWS,5c.REGLOWS", 0,
                                      false, false);
  assert(validated_moves_get_number_of_moves(vms) == 2);
  assert(move_get_score(validated_moves_get_move(vms, 0)) == 12);
  assert(move_get_score(validated_moves_get_move(vms, 1)) == 11);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(game, VS_MATT,
                                      "15c.AVENGED,1L.FA,K9.TAEL,b10.BEHEAD,K9."
                                      "TAE,K11.ED,K10.AE,K11.ETA,B9.BATHED",
                                      0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 9);
  assert(move_get_score(validated_moves_get_move(vms, 0)) == 12);
  assert(move_get_score(validated_moves_get_move(vms, 1)) == 5);
  assert(move_get_score(validated_moves_get_move(vms, 2)) == 38);
  assert(move_get_score(validated_moves_get_move(vms, 3)) == 36);
  assert(move_get_score(validated_moves_get_move(vms, 4)) == 34);
  assert(move_get_score(validated_moves_get_move(vms, 5)) == 33);
  assert(move_get_score(validated_moves_get_move(vms, 6)) == 30);
  assert(move_get_score(validated_moves_get_move(vms, 7)) == 34);
  assert(move_get_score(validated_moves_get_move(vms, 8)) == 28);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      game, VS_JEREMY, "14B.hEaDWORDS,14B.hEaDWORD", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 2);
  assert(move_get_score(validated_moves_get_move(vms, 0)) == 106);
  assert(move_get_score(validated_moves_get_move(vms, 1)) == 38);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(
      game, VS_OXY, "A1.OXYPHENBUTAZONE,A12.ZONE", 0, false, false);
  assert(validated_moves_get_number_of_moves(vms) == 2);
  assert(move_get_score(validated_moves_get_move(vms, 0)) == 1780);
  assert(move_get_score(validated_moves_get_move(vms, 1)) == 160);
  validated_moves_destroy(vms);

  vms = assert_validated_move_success(game, EMPTY_CGP, "8C.OVERDOG", 0, false,
                                      false);
  assert(validated_moves_get_number_of_moves(vms) == 1);
  assert(move_get_score(validated_moves_get_move(vms, 0)) == 82);
  validated_moves_destroy(vms);

  game_destroy(game);
  config_destroy(config);
}

void test_validated_move_distinct_kwg(void) {
  Config *config =
      config_create_or_die("set -l1 CSW21 -l2 NWL20 -s1 equity -s2 equity "
                           "-r1 best -r2 best -numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  MoveList *move_list = move_list_create(1);

  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);

  // Play SPORK, better than best NWL move of PORKS
  ValidatedMoves *vms =
      validated_moves_create(game, 0, "8H.SPORK", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  rack_set_to_string(ld, player0_rack, "KOPRRSS");
  generate_moves_for_game(game, 0, move_list);
  assert_move(game, move_list, NULL, 0, "8H SPORK 32");
  play_move(move_list_get_move(move_list, 0), game, NULL);

  // Play SCHIZIER, better than best CSW word of SCHERZI
  vms = validated_moves_create(game, 1, "H8.SCHIZIER", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  assert(move_get_score(validated_moves_get_move(vms, 0)) == 146);
  validated_moves_destroy(vms);

  vms = validated_moves_create(game, 1, "M8.SCHERZI", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  rack_set_to_string(ld, player1_rack, "CEHIIRZ");
  generate_moves_for_game(game, 0, move_list);
  assert_move(game, move_list, NULL, 0, "H8 (S)CHIZIER 146");
  play_move(move_list_get_move(move_list, 0), game, NULL);

  // Play WIGGLY, not GOLLYWOG because that's NWL only
  vms = validated_moves_create(game, 0, "11G.WIGGLY", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  vms = validated_moves_create(game, 0, "J2.GOLLYWOG", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  // print_board(game_get_board(game));
  rack_set_to_string(ld, player0_rack, "GGLLOWY");
  generate_moves_for_game(game, 0, move_list);
  assert_move(game, move_list, NULL, 0, "11G W(I)GGLY 28");
  play_move(move_list_get_move(move_list, 0), game, NULL);

  // Play 13C QUEAS(I)ER, not L3 SQUEA(K)ER(Y) because that's CSW only
  vms = validated_moves_create(game, 1, "13C.QUEASIER", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  vms = validated_moves_create(game, 1, "L3.SQUEAKERY", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  rack_set_to_string(ld, player1_rack, "AEEQRSU");
  generate_moves_for_game(game, 0, move_list);
  assert_move(game, move_list, NULL, 0, "13C QUEAS(I)ER 88");

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void test_validated_move_wordsmog_phonies(void) {
  Config *config =
      config_create_or_die("set -lex CSW21_alpha -s1 equity -s2 equity "
                           "-r1 best -r2 best -numplays 1 -var wordsmog");
  Game *game = config_game_create(config);

  ValidatedMoves *vms =
      validated_moves_create(game, 0, "8H.TRONGLE", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  load_cgp_or_die(game, ENTASIS_OPENING_CGP);

  vms = validated_moves_create(game, 0, "7C.DUOGRNA", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
  validated_moves_destroy(vms);

  vms = validated_moves_create(game, 0, "7C.DUORENA", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  validated_moves_destroy(vms);

  game_destroy(game);
  config_destroy(config);
}

void test_validated_move_many(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);

  ValidatedMoves *vms = assert_validated_move_success(
      game, EMPTY_CGP, "  pass ,  ex.4  ,  ex.ABC,  8d.JIHAD , h8.QIS ", 0,
      false, false);
  assert(validated_moves_get_number_of_moves(vms) == 5);
  assert(move_get_type(validated_moves_get_move(vms, 0)) == GAME_EVENT_PASS);
  assert(move_get_type(validated_moves_get_move(vms, 1)) ==
         GAME_EVENT_EXCHANGE);
  assert(move_get_type(validated_moves_get_move(vms, 2)) ==
         GAME_EVENT_EXCHANGE);
  assert(move_get_type(validated_moves_get_move(vms, 3)) ==
         GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move_get_type(validated_moves_get_move(vms, 4)) ==
         GAME_EVENT_TILE_PLACEMENT_MOVE);
  validated_moves_destroy(vms);

  vms = validated_moves_create(game, 0, "pass.ABC.AB,ex.4,ex.ABC.DEF,8h.VVU",
                               false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_EXCESS_PASS_FIELDS);
  validated_moves_destroy(vms);

  vms = validated_moves_create(game, 0, "pass,ex.4,ex.ABC.DEF,8h.VVU", false,
                               true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_TILES_PLAYED_NOT_IN_RACK);
  validated_moves_destroy(vms);

  vms = validated_moves_create(game, 0, "pass,ex.4,ex.ABC.ABCDEF,8h.VVU", false,
                               true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_PHONY_WORD_FORMED);
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
