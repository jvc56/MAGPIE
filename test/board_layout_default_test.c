#include <assert.h>
#include <stdint.h>

#include "../src/def/board_defs.h"
#include "../src/def/static_eval_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/bonus_square.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"

#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"

#include "../src/util/io_util.h"

#include "test_constants.h"
#include "test_util.h"

void assert_bonus_square(const BonusSquare bonus_square,
                         const uint8_t expected_value) {
  const uint8_t word_multiplier = (expected_value >> 4);
  const uint8_t letter_multiplier = expected_value & 0xF;
  if (bonus_square_get_word_multiplier(bonus_square) != word_multiplier) {
    log_fatal("word multiplier mismatch: expected %d, got %d", word_multiplier,
              bonus_square_get_word_multiplier(bonus_square));
  }
  if (bonus_square_get_letter_multiplier(bonus_square) != letter_multiplier) {
    log_fatal("letter multiplier mismatch: expected %d, got %d",
              letter_multiplier,
              bonus_square_get_letter_multiplier(bonus_square));
  }
}

void test_board_layout_success(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);

  Board *board = game_get_board(game);

  for (int i = 0; i < 2; i++) {
    assert_bonus_square(board_get_bonus_square(board, 0, 1), 0x11);
    assert_bonus_square(board_get_bonus_square(board, 1, 0), 0x11);
    assert_bonus_square(board_get_bonus_square(board, 4, 3), 0x11);
    assert_bonus_square(board_get_bonus_square(board, 3, 4), 0x11);
    assert_bonus_square(board_get_bonus_square(board, 9, 4), 0x11);
    assert_bonus_square(board_get_bonus_square(board, 4, 9), 0x11);
    assert_bonus_square(board_get_bonus_square(board, 1, 14), 0x11);
    assert_bonus_square(board_get_bonus_square(board, 14, 1), 0x11);
    assert_bonus_square(board_get_bonus_square(board, 10, 13), 0x11);
    assert_bonus_square(board_get_bonus_square(board, 13, 10), 0x11);

    assert_bonus_square(board_get_bonus_square(board, 0, 0), 0x31);
    assert_bonus_square(board_get_bonus_square(board, 0, 7), 0x31);
    assert_bonus_square(board_get_bonus_square(board, 7, 0), 0x31);
    assert_bonus_square(board_get_bonus_square(board, 7, 14), 0x31);
    assert_bonus_square(board_get_bonus_square(board, 14, 7), 0x31);
    assert_bonus_square(board_get_bonus_square(board, 0, 14), 0x31);
    assert_bonus_square(board_get_bonus_square(board, 14, 0), 0x31);
    assert_bonus_square(board_get_bonus_square(board, 14, 14), 0x31);

    assert_bonus_square(board_get_bonus_square(board, 5, 1), 0x13);
    assert_bonus_square(board_get_bonus_square(board, 9, 1), 0x13);
    assert_bonus_square(board_get_bonus_square(board, 9, 5), 0x13);
    assert_bonus_square(board_get_bonus_square(board, 9, 9), 0x13);
    assert_bonus_square(board_get_bonus_square(board, 9, 13), 0x13);

    assert_bonus_square(board_get_bonus_square(board, 6, 2), 0x12);
    assert_bonus_square(board_get_bonus_square(board, 7, 3), 0x12);
    assert_bonus_square(board_get_bonus_square(board, 2, 8), 0x12);
    assert_bonus_square(board_get_bonus_square(board, 3, 14), 0x12);
    assert_bonus_square(board_get_bonus_square(board, 11, 14), 0x12);
    assert_bonus_square(board_get_bonus_square(board, 6, 6), 0x12);
    assert_bonus_square(board_get_bonus_square(board, 8, 8), 0x12);

    assert_bonus_square(board_get_bonus_square(board, 1, 1), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 2, 2), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 3, 3), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 7, 7), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 13, 1), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 12, 2), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 11, 3), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 3, 11), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 2, 12), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 1, 13), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 13, 13), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 12, 12), 0x21);
    assert_bonus_square(board_get_bonus_square(board, 11, 11), 0x21);
    board_transpose(board);
  }

  game_destroy(game);
  config_destroy(config);
}

void test_board_layout_error(void) {
  assert_board_layout_error(DEFAULT_TEST_DATA_PATH, "malformed_start_coords15",
                            ERROR_STATUS_BOARD_LAYOUT_MALFORMED_START_COORDS);
  assert_board_layout_error(
      DEFAULT_TEST_DATA_PATH, "out_of_bounds_start_coords15",
      ERROR_STATUS_BOARD_LAYOUT_OUT_OF_BOUNDS_START_COORDS);
  assert_board_layout_error(DEFAULT_TEST_DATA_PATH, "invalid_number_of_rows15",
                            ERROR_STATUS_BOARD_LAYOUT_INVALID_NUMBER_OF_ROWS);
  assert_board_layout_error(DEFAULT_TEST_DATA_PATH, "standard21",
                            ERROR_STATUS_BOARD_LAYOUT_INVALID_NUMBER_OF_ROWS);
  assert_board_layout_error(DEFAULT_TEST_DATA_PATH, "invalid_number_of_cols15",
                            ERROR_STATUS_BOARD_LAYOUT_INVALID_NUMBER_OF_COLS);
  assert_board_layout_error(DEFAULT_TEST_DATA_PATH, "invalid_bonus_square15",
                            ERROR_STATUS_BOARD_LAYOUT_INVALID_BONUS_SQUARE);
}

void assert_opening_penalties(Game *game, const char *data_paths,
                              const char *layout, const char *rack_string,
                              int score, Equity equity) {
  load_game_with_test_board(game, data_paths, layout);
  const Player *player =
      game_get_player(game, game_get_player_on_turn_index(game));
  Rack *player_rack = player_get_rack(player);
  MoveList *move_list = move_list_create(1);
  rack_set_to_string(game_get_ld(game), player_rack, rack_string);
  const MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .max_equity_diff = 0,
  };
  generate_moves_for_game(&move_gen_args);
  const Move *move = move_list_get_move(move_list, 0);
  assert_move_score(move, score);
  assert_move_equity_exact(move, equity);
  move_list_destroy(move_list);
}

void test_board_layout_correctness(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  const Board *board = game_get_board(game);
  const char *data_paths = config_get_data_paths(config);

  assert(board_get_anchor(board, 7, 7, BOARD_HORIZONTAL_DIRECTION));
  assert(!board_get_anchor(board, 7, 7, BOARD_VERTICAL_DIRECTION));

  load_game_with_test_board(game, data_paths, "asymmetric15");

  assert(board_get_anchor(board, 7, 7, BOARD_HORIZONTAL_DIRECTION));
  assert(board_get_anchor(board, 7, 7, BOARD_VERTICAL_DIRECTION));

  load_game_with_test_board(game, data_paths, "3_2_start_coords_15");

  assert(board_get_anchor(board, 3, 2, BOARD_HORIZONTAL_DIRECTION));
  assert(board_get_anchor(board, 3, 2, BOARD_VERTICAL_DIRECTION));

  load_game_with_test_board(game, data_paths, "quadruple_word_opening15");

  assert_validated_and_generated_moves(game, "QUONKES", "8D", "QUONK", 112,
                                       false);

  load_game_with_test_board(game, data_paths, "quadruple_letter_opening15");

  assert_validated_and_generated_moves(game, "TALAQES", "8H", "TALAQ", 88,
                                       false);

  load_game_with_test_board(game, data_paths, "3_2_start_coords_15");

  assert_validated_and_generated_moves(game, "AGUIZED", "C3", "AGUIZED", 110,
                                       false);

  load_game_with_test_board(game, data_paths, "0_0_start_coords_15");

  assert_validated_and_generated_moves(game, "QUIZEST", "1A", "QUIZ", 96,
                                       false);

  load_game_with_test_board(game, data_paths, "0_14_start_coords_15");

  assert_validated_and_generated_moves(game, "PUCKEST", "O1", "PUCK", 51,
                                       false);
  assert_validated_and_generated_moves(game, "JOUKERS", "1L", "JOUK", 69,
                                       false);

  load_game_with_test_board(game, data_paths, "14_0_start_coords_15");

  assert_validated_and_generated_moves(game, "PUCKEST", "15A", "PUCK", 51,
                                       false);
  assert_validated_and_generated_moves(game, "JOUKERS", "A12", "JOUK", 69,
                                       false);

  load_game_with_test_board(game, data_paths, "14_14_start_coords_15");

  assert_validated_and_generated_moves(game, "PIGGIEI", "15J", "PIGGIE", 36,
                                       false);

  // Test bricks
  load_game_with_test_board(game, data_paths, "start_is_bricked_15");

  // No tile placement moves have been generated since the start is bricked,
  // therefore no placement of BUSUUTI is valid in this position.
  assert_validated_and_generated_moves(game, "BUSUUTI", "exch", "BUUU", 0,
                                       false);

  load_game_with_test_board(game, data_paths, "8D_is_bricked_15");

  // Best move is at H4 because:
  // - the double letter square at 8D is bricked
  // - the board is no longer symmetrical about the diagonal, so vertical
  //   plays on the start square are allowed.
  assert_validated_and_generated_moves(game, "QUONKES", "H4", "QUONK", 56,
                                       false);

  load_game_with_test_board(game, data_paths, "8G_and_7H_are_bricked_15");

  // Best move is at 8H because:
  // - the square at 8G and 7H are bricked preventing access
  //   to the 8D and 4H double letter squares.
  // - the board is symmetrical about the diagonal, so only
  //   horizontal plays on the start square are allowed.
  assert_validated_and_generated_moves(game, "QUONKES", "8H", "QUONK", 46,
                                       false);

  load_game_with_test_board(game, data_paths, "5_by_5_bricked_box_15");

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

  load_game_with_test_board(game, data_paths, "single_row_15");
  assert_validated_and_generated_moves(game, "KGOTLAT", "8D", "KGOTLA", 32,
                                       true);
  // The only legal play is extending KGOTLA to LEKGOTLA
  assert_validated_and_generated_moves(game, "RATLINE", "8B", "LE(KGOTLA)", 13,
                                       true);

  // Assume players are using the same KLV
  assert_opening_penalties(game, data_paths, "standard15", "QUIRKED", 112,
                           int_to_equity(112) + OPENING_HOTSPOT_PENALTY);
  assert_opening_penalties(game, data_paths, "no_bonus_squares_15", "QUIRKED",
                           71, int_to_equity(71));
  assert_opening_penalties(game, data_paths, "no_bonus_squares_15", "EUOUAES",
                           57, int_to_equity(57));
  assert_opening_penalties(
      game, data_paths, "many_opening_hotspots_15", "EUOUAES", 66,
      int_to_equity(66) + ((OPENING_HOTSPOT_PENALTY / 2) * 12));
  assert_opening_penalties(
      game, data_paths, "many_opening_hotspots_vertical_15", "EUOUAES", 66,
      int_to_equity(66) + ((OPENING_HOTSPOT_PENALTY / 2) * 5));

  load_game_with_test_board(game, data_paths,
                            "many_opening_hotspots_vertical_15");

  Equity opening_penalties[BOARD_DIM * 2];
  board_copy_opening_penalties(board, opening_penalties);
  assert(opening_penalties[0] == 0);
  assert(opening_penalties[1] == 0);
  assert(opening_penalties[2] == OPENING_HOTSPOT_PENALTY * 3);
  assert(opening_penalties[3] == OPENING_HOTSPOT_PENALTY * 3);
  assert(opening_penalties[4] == OPENING_HOTSPOT_PENALTY * 3);
  assert(opening_penalties[5] == OPENING_HOTSPOT_PENALTY * 3);
  assert(opening_penalties[6] == OPENING_HOTSPOT_PENALTY * 3);
  assert(opening_penalties[7] == 0);
  assert(opening_penalties[8] == OPENING_HOTSPOT_PENALTY * 1.5);
  assert(opening_penalties[9] == OPENING_HOTSPOT_PENALTY * 1.5);
  assert(opening_penalties[10] == OPENING_HOTSPOT_PENALTY * 1.5);
  assert(opening_penalties[15] == OPENING_HOTSPOT_PENALTY * 3);
  assert(opening_penalties[16] == OPENING_HOTSPOT_PENALTY * 3);
  assert(opening_penalties[17] == OPENING_HOTSPOT_PENALTY * 3);
  assert(opening_penalties[22] == 0);
  assert(opening_penalties[23] == OPENING_HOTSPOT_PENALTY * 1.5);
  assert(opening_penalties[24] == 0);

  // Validate play out of bounds
  load_game_with_test_board(game, data_paths, "standard15");

  load_cgp_or_die(game, ENTASIS_OPENING_CGP);

  ValidatedMoves *vms = validated_moves_create_and_assert_status(
      game, 0, "7J.FRAWZEY", false, false, false,
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_OUT_OF_BOUNDS);
  validated_moves_destroy(vms);

  // Validate play over block
  load_game_with_test_board(game, data_paths, "5_by_5_bricked_box_15");

  vms = validated_moves_create_and_assert_status(
      game, 0, "8H.FRAWZEY", false, false, false,
      ERROR_STATUS_MOVE_VALIDATION_TILES_PLAYED_OVER_BRICK);
  validated_moves_destroy(vms);

  game_destroy(game);
  config_destroy(config);
}

void test_board_layout_default(void) {
  test_board_layout_success();
  test_board_layout_error();
  test_board_layout_correctness();
}