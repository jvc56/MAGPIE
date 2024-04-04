#include <assert.h>

#include "../../src/ent/board.h"
#include "../../src/ent/board_layout.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"
#include "../../src/ent/move.h"
#include "../../src/ent/validated_move.h"

#include "../../src/impl/gameplay.h"
#include "../../src/impl/move_gen.h"

#include "test_constants.h"
#include "test_util.h"

void test_board_layout_success() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);

  assert(bonus_square_value_to_char(0x12) == '\'');
  assert(bonus_square_value_to_char(0x21) == '-');
  assert(bonus_square_value_to_char(0x13) == '"');
  assert(bonus_square_value_to_char(0x31) == '=');

  assert(bonus_square_char_to_value('\'') == 0x12);
  assert(bonus_square_char_to_value('-') == 0x21);
  assert(bonus_square_char_to_value('"') == 0x13);
  assert(bonus_square_char_to_value('=') == 0x31);

  Board *board = game_get_board(game);

  for (int i = 0; i < 2; i++) {
    assert(board_get_bonus_square(board, 0, 1) == 0x11);
    assert(board_get_bonus_square(board, 1, 0) == 0x11);
    assert(board_get_bonus_square(board, 4, 3) == 0x11);
    assert(board_get_bonus_square(board, 3, 4) == 0x11);
    assert(board_get_bonus_square(board, 9, 4) == 0x11);
    assert(board_get_bonus_square(board, 4, 9) == 0x11);
    assert(board_get_bonus_square(board, 1, 14) == 0x11);
    assert(board_get_bonus_square(board, 14, 1) == 0x11);
    assert(board_get_bonus_square(board, 10, 13) == 0x11);
    assert(board_get_bonus_square(board, 13, 10) == 0x11);

    assert(board_get_bonus_square(board, 0, 0) == 0x31);
    assert(board_get_bonus_square(board, 0, 7) == 0x31);
    assert(board_get_bonus_square(board, 7, 0) == 0x31);
    assert(board_get_bonus_square(board, 7, 14) == 0x31);
    assert(board_get_bonus_square(board, 14, 7) == 0x31);
    assert(board_get_bonus_square(board, 0, 14) == 0x31);
    assert(board_get_bonus_square(board, 14, 0) == 0x31);
    assert(board_get_bonus_square(board, 14, 14) == 0x31);

    assert(board_get_bonus_square(board, 5, 1) == 0x13);
    assert(board_get_bonus_square(board, 9, 1) == 0x13);
    assert(board_get_bonus_square(board, 9, 5) == 0x13);
    assert(board_get_bonus_square(board, 9, 9) == 0x13);
    assert(board_get_bonus_square(board, 9, 13) == 0x13);

    assert(board_get_bonus_square(board, 6, 2) == 0x12);
    assert(board_get_bonus_square(board, 7, 3) == 0x12);
    assert(board_get_bonus_square(board, 2, 8) == 0x12);
    assert(board_get_bonus_square(board, 3, 14) == 0x12);
    assert(board_get_bonus_square(board, 11, 14) == 0x12);
    assert(board_get_bonus_square(board, 6, 6) == 0x12);
    assert(board_get_bonus_square(board, 8, 8) == 0x12);

    assert(board_get_bonus_square(board, 1, 1) == 0x21);
    assert(board_get_bonus_square(board, 2, 2) == 0x21);
    assert(board_get_bonus_square(board, 3, 3) == 0x21);
    assert(board_get_bonus_square(board, 7, 7) == 0x21);
    assert(board_get_bonus_square(board, 13, 1) == 0x21);
    assert(board_get_bonus_square(board, 12, 2) == 0x21);
    assert(board_get_bonus_square(board, 11, 3) == 0x21);
    assert(board_get_bonus_square(board, 3, 11) == 0x21);
    assert(board_get_bonus_square(board, 2, 12) == 0x21);
    assert(board_get_bonus_square(board, 1, 13) == 0x21);
    assert(board_get_bonus_square(board, 13, 13) == 0x21);
    assert(board_get_bonus_square(board, 12, 12) == 0x21);
    assert(board_get_bonus_square(board, 11, 11) == 0x21);
    board_transpose(board);
  }

  game_destroy(game);
  config_destroy(config);
}

void test_board_layout_error() {
  assert_board_layout_error("malformed_start_coords15.txt",
                            BOARD_LAYOUT_LOAD_STATUS_MALFORMED_START_COORDS);
  assert_board_layout_error(
      "out_of_bounds_start_coords15.txt",
      BOARD_LAYOUT_LOAD_STATUS_OUT_OF_BOUNDS_START_COORDS);
  assert_board_layout_error("invalid_number_of_rows15.txt",
                            BOARD_LAYOUT_LOAD_STATUS_INVALID_NUMBER_OF_ROWS);
  assert_board_layout_error("standard21.txt",
                            BOARD_LAYOUT_LOAD_STATUS_INVALID_NUMBER_OF_ROWS);
  assert_board_layout_error("invalid_number_of_cols15.txt",
                            BOARD_LAYOUT_LOAD_STATUS_INVALID_NUMBER_OF_COLS);
  assert_board_layout_error("invalid_bonus_square15.txt",
                            BOARD_LAYOUT_LOAD_STATUS_INVALID_BONUS_SQUARE);
}

void assert_opening_penalties(Game *game, const char *layout,
                              const char *rack_string, int score,
                              double equity) {
  load_game_with_test_board(game, layout);
  Player *player = game_get_player(game, game_get_player_on_turn_index(game));
  Rack *player_rack = player_get_rack(player);
  MoveList *move_list = move_list_create(1);
  rack_set_to_string(game_get_ld(game), player_rack, rack_string);
  generate_moves_for_game(game, 0, move_list);
  Move *move = move_list_get_move(move_list, 0);
  assert(move_get_score(move) == score);
  assert(within_epsilon_for_equity(move_get_equity(move), equity));
  move_list_destroy(move_list);
}

void test_board_layout_correctness() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Board *board = game_get_board(game);

  assert(board_get_anchor(board, 7, 7, BOARD_HORIZONTAL_DIRECTION));
  assert(!board_get_anchor(board, 7, 7, BOARD_VERTICAL_DIRECTION));

  load_game_with_test_board(game, "asymmetric15.txt");

  assert(board_get_anchor(board, 7, 7, BOARD_HORIZONTAL_DIRECTION));
  assert(board_get_anchor(board, 7, 7, BOARD_VERTICAL_DIRECTION));

  load_game_with_test_board(game, "3_2_start_coords_15.txt");

  assert(board_get_anchor(board, 3, 2, BOARD_HORIZONTAL_DIRECTION));
  assert(board_get_anchor(board, 3, 2, BOARD_VERTICAL_DIRECTION));

  load_game_with_test_board(game, "quadruple_word_opening15.txt");

  assert_validated_and_generated_moves(game, "QUONKES", "8D", "QUONK", 112,
                                       false);

  load_game_with_test_board(game, "quadruple_letter_opening15.txt");

  assert_validated_and_generated_moves(game, "TALAQES", "8H", "TALAQ", 88,
                                       false);

  load_game_with_test_board(game, "3_2_start_coords_15.txt");

  assert_validated_and_generated_moves(game, "AGUIZED", "C3", "AGUIZED", 110,
                                       false);

  load_game_with_test_board(game, "0_0_start_coords_15.txt");

  assert_validated_and_generated_moves(game, "QUIZEST", "1A", "QUIZ", 96,
                                       false);

  load_game_with_test_board(game, "0_14_start_coords_15.txt");

  assert_validated_and_generated_moves(game, "PUCKEST", "O1", "PUCK", 51,
                                       false);
  assert_validated_and_generated_moves(game, "JOUKERS", "1L", "JOUK", 69,
                                       false);

  load_game_with_test_board(game, "14_0_start_coords_15.txt");

  assert_validated_and_generated_moves(game, "PUCKEST", "15A", "PUCK", 51,
                                       false);
  assert_validated_and_generated_moves(game, "JOUKERS", "A12", "JOUK", 69,
                                       false);

  load_game_with_test_board(game, "14_14_start_coords_15.txt");

  assert_validated_and_generated_moves(game, "PIGGIEI", "15J", "PIGGIE", 36,
                                       false);

  // Test bricks
  load_game_with_test_board(game, "start_is_bricked_15.txt");

  // No tile placement moves have been generated since the start is bricked,
  // therefore no placement of BUSUUTI is valid in this position.
  assert_validated_and_generated_moves(game, "BUSUUTI", "exch", "BUUU", 0,
                                       false);

  load_game_with_test_board(game, "8D_is_bricked_15.txt");

  // Best move is at H4 because:
  // - the double letter square at 8D is bricked
  // - the board is no longer symmetrical about the diagonal, so vertical
  //   plays on the start square are allowed.
  assert_validated_and_generated_moves(game, "QUONKES", "H4", "QUONK", 56,
                                       false);

  load_game_with_test_board(game, "8G_and_7H_are_bricked_15.txt");

  // Best move is at 8H because:
  // - the square at 8G and 7H are bricked preventing access
  //   to the 8D and 4H double letter squares.
  // - the board is symmetrical about the diagonal, so only
  //   horizontal plays on the start square are allowed.
  assert_validated_and_generated_moves(game, "QUONKES", "8H", "QUONK", 46,
                                       false);

  load_game_with_test_board(game, "5_by_5_bricked_box_15.txt");

  assert_validated_and_generated_moves(game, "FRAWZEY", "8F", "AWFY", 26, true);

  assert_validated_and_generated_moves(game, "BINGERS", "7F", "BEING", 31,
                                       true);
  assert_validated_and_generated_moves(game, "NARASES", "6F", "ANSA", 31, true);
  assert_validated_and_generated_moves(game, "KARATES", "F6", "(ABA)KA", 13,
                                       true);
  assert_validated_and_generated_moves(game, "CRETINS", "H6", "(SIF)T", 7,
                                       true);
  assert_validated_and_generated_moves(game, "SEXTAIN", "10H", "SAX", 34, true);
  assert_validated_and_generated_moves(game, "ENTREES", "J9", "E(X)", 9, true);
  // There are no more legal plays available other than exchanging.
  assert_validated_and_generated_moves(game, "AEINT??", "exch", "AT", 0, true);

  load_game_with_test_board(game, "single_row_15.txt");
  assert_validated_and_generated_moves(game, "KGOTLAT", "8D", "KGOTLA", 32,
                                       true);
  // The only legal play is extending KGOTLA to LEKGOTLA
  assert_validated_and_generated_moves(game, "RATLINE", "8B", "LE(KGOTLA)", 13,
                                       true);

  // Assume players are using the same KLV
  assert_opening_penalties(game, "standard15.txt", "QUIRKED", 112,
                           112 + OPENING_HOTSPOT_PENALTY);
  assert_opening_penalties(game, "no_bonus_squares_15.txt", "QUIRKED", 71, 71);
  assert_opening_penalties(game, "no_bonus_squares_15.txt", "EUOUAES", 57, 57);
  assert_opening_penalties(game, "many_opening_hotspots_15.txt", "EUOUAES", 66,
                           66 + ((OPENING_HOTSPOT_PENALTY / 2) * 12));
  assert_opening_penalties(game, "many_opening_hotspots_vertical_15.txt",
                           "EUOUAES", 66,
                           66 + ((OPENING_HOTSPOT_PENALTY / 2) * 5));

  double opening_penalties[BOARD_DIM * 2];
  board_copy_opening_penalties(board, opening_penalties);
  assert(within_epsilon_for_equity(opening_penalties[0], 0));
  assert(within_epsilon_for_equity(opening_penalties[1], 0));
  assert(within_epsilon_for_equity(opening_penalties[2],
                                   OPENING_HOTSPOT_PENALTY * 3));
  assert(within_epsilon_for_equity(opening_penalties[3],
                                   OPENING_HOTSPOT_PENALTY * 3));
  assert(within_epsilon_for_equity(opening_penalties[4],
                                   OPENING_HOTSPOT_PENALTY * 3));
  assert(within_epsilon_for_equity(opening_penalties[5],
                                   OPENING_HOTSPOT_PENALTY * 3));
  assert(within_epsilon_for_equity(opening_penalties[6],
                                   OPENING_HOTSPOT_PENALTY * 3));
  assert(within_epsilon_for_equity(opening_penalties[7], 0));
  assert(within_epsilon_for_equity(opening_penalties[8],
                                   OPENING_HOTSPOT_PENALTY * 1.5));
  assert(within_epsilon_for_equity(opening_penalties[9],
                                   OPENING_HOTSPOT_PENALTY * 1.5));
  assert(within_epsilon_for_equity(opening_penalties[10],
                                   OPENING_HOTSPOT_PENALTY * 1.5));
  assert(within_epsilon_for_equity(opening_penalties[15],
                                   OPENING_HOTSPOT_PENALTY * 3));
  assert(within_epsilon_for_equity(opening_penalties[16],
                                   OPENING_HOTSPOT_PENALTY * 3));
  assert(within_epsilon_for_equity(opening_penalties[17],
                                   OPENING_HOTSPOT_PENALTY * 3));
  assert(within_epsilon_for_equity(opening_penalties[17],
                                   OPENING_HOTSPOT_PENALTY * 3));
  assert(within_epsilon_for_equity(opening_penalties[22], 0));
  assert(within_epsilon_for_equity(opening_penalties[23],
                                   OPENING_HOTSPOT_PENALTY * 1.5));
  assert(within_epsilon_for_equity(opening_penalties[24], 0));

  // Validate play out of bounds
  load_game_with_test_board(game, "standard15.txt");

  load_cgp_or_die(game, ENTASIS_OPENING_CGP);

  ValidatedMoves *vms =
      validated_moves_create(game, 0, "7J.FRAWZEY", false, false);
  move_validation_status_t vms_error_status =
      validated_moves_get_validation_status(vms);
  assert(vms_error_status == MOVE_VALIDATION_STATUS_TILES_PLAYED_OUT_OF_BOUNDS);
  validated_moves_destroy(vms);

  // Validate play over block
  load_game_with_test_board(game, "5_by_5_bricked_box_15.txt");

  vms = validated_moves_create(game, 0, "8H.FRAWZEY", false, false);
  vms_error_status = validated_moves_get_validation_status(vms);
  assert(vms_error_status == MOVE_VALIDATION_STATUS_TILES_PLAYED_OVER_BRICK);
  validated_moves_destroy(vms);

  game_destroy(game);
  config_destroy(config);
}

void test_board_layout_default() {
  test_board_layout_success();
  test_board_layout_error();
  test_board_layout_correctness();
}