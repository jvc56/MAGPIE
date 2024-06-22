#include <assert.h>
#include <stdint.h>

#include "../../src/def/board_defs.h"

#include "../../src/ent/board.h"
#include "../../src/ent/game.h"
#include "../../src/ent/validated_move.h"
#include "../../src/impl/config.h"

#include "../../src/impl/cgp.h"
#include "../../src/impl/gameplay.h"

#include "board_test.h"
#include "test_constants.h"
#include "test_util.h"

void test_board_cross_set_for_cross_set_index(Game *game, int cross_set_index) {
  // Test cross set
  Board *board = game_get_board(game);

  board_clear_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                        cross_set_index);
  board_set_cross_set_letter(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                             cross_set_index, 13);
  assert(board_get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                             cross_set_index) == 8192);
  board_set_cross_set_letter(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                             cross_set_index, 0);
  assert(board_get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                             cross_set_index) == 8193);

  uint64_t cs = board_get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                                    cross_set_index);
  assert(!board_is_letter_allowed_in_cross_set(cs, 1));
  assert(board_is_letter_allowed_in_cross_set(cs, 0));
  assert(!board_is_letter_allowed_in_cross_set(cs, 14));
  assert(board_is_letter_allowed_in_cross_set(cs, 13));
  assert(!board_is_letter_allowed_in_cross_set(cs, 12));

  board_set_cross_set_with_blank(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                                 cross_set_index, 0);
  cs = board_get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                           cross_set_index);
  assert(!board_is_letter_allowed_in_cross_set(cs, 0));
  assert(!board_is_letter_allowed_in_cross_set(cs, 1));

  board_set_cross_set_with_blank(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                                 cross_set_index, get_cross_set_bit(20));
  cs = board_get_cross_set(board, 0, 0, BOARD_HORIZONTAL_DIRECTION,
                           cross_set_index);
  assert(board_is_letter_allowed_in_cross_set(cs, 0));
  assert(!board_is_letter_allowed_in_cross_set(cs, 1));
  assert(board_is_letter_allowed_in_cross_set(cs, 20));
}

void test_board_reset(Board *board) {
  board_reset(board);
  assert(!board_get_transposed(board));
  assert(board_get_tiles_played(board) == 0);
  for (int t = 0; t < 2; t++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      if (t == 0) {
        if (row == 7) {
          assert(board_get_number_of_row_anchors(board, row, 0) == 1);
          assert(board_get_number_of_row_anchors(board, row, 1) == 0);
        } else {
          assert(board_get_number_of_row_anchors(board, row, 0) == 0);
          assert(board_get_number_of_row_anchors(board, row, 1) == 0);
        }
      }
      for (int col = 0; col < BOARD_DIM; col++) {
        assert(board_get_letter(board, row, col) ==
               ALPHABET_EMPTY_SQUARE_MARKER);
        for (int dir = 0; dir < 2; dir++) {
          if (((t == 0 && dir == 0) || (t == 1 && dir == 1)) && row == 7 &&
              col == 7) {
            assert(board_get_anchor(board, row, col, dir));
          } else {
            assert(!board_get_anchor(board, row, col, dir));
          }
          assert(!board_get_is_cross_word(board, row, col, dir));
          // For now, assume all boards tested in this method
          // share the same lexicon
          for (int cross_index = 0; cross_index < 2; cross_index++) {
            assert(board_get_cross_set(board, row, col, dir, cross_index) ==
                   TRIVIAL_CROSS_SET);
            assert(board_get_cross_score(board, row, col, dir, cross_index) ==
                   0);
          }
        }
      }
    }
    board_transpose(board);
  }
}

void test_board_all() {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  Rack *player0_rack = player_get_rack(player0);
  Rack *player1_rack = player_get_rack(player1);
  Board *board = game_get_board(game);
  Board *board2 = board_duplicate(board);

  game_load_cgp(game, VS_OXY);

  assert(board_are_bonus_squares_symmetric_by_transposition(board));
  assert(board_are_bonus_squares_symmetric_by_transposition(board2));

  assert(board_get_is_cross_word(board, 0, 0, 1));
  assert(board_get_is_cross_word(board, 1, 0, 1));
  assert(board_get_is_cross_word(board, 3, 0, 1));
  assert(board_get_is_cross_word(board, 4, 3, 1));

  assert(!board_get_is_cross_word(board, 2, 3, 1));
  assert(!board_get_is_cross_word(board, 2, 4, 1));
  assert(!board_get_is_cross_word(board, 2, 5, 1));
  assert(!board_get_is_cross_word(board, 2, 6, 1));
  assert(!board_get_is_cross_word(board, 2, 7, 1));
  assert(!board_get_is_cross_word(board, 2, 8, 1));
  assert(!board_get_is_cross_word(board, 4, 4, 1));
  assert(!board_get_is_cross_word(board, 4, 5, 1));
  assert(!board_get_is_cross_word(board, 4, 6, 1));
  assert(!board_get_is_cross_word(board, 4, 7, 1));
  assert(!board_get_is_cross_word(board, 4, 8, 1));

  game_reset(game);

  game_load_cgp(game, BOTTOM_LEFT_RE_CGP);

  assert(board_get_is_cross_word(board, 13, 0, 0));
  assert(board_get_is_cross_word(board, 13, 1, 0));

  game_reset(game);

  rack_set_to_string(ld, player0_rack, "KOPRRSS");
  ValidatedMoves *vms =
      validated_moves_create(game, 0, "8H.SPORK", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  play_move(validated_moves_get_move(vms, 0), game, NULL);
  validated_moves_destroy(vms);

  // Play SCHIZIER, better than best CSW word of SCHERZI
  rack_set_to_string(ld, player1_rack, "CAURING");
  vms = validated_moves_create(game, 1, "H8.SCAURING", false, true, false);
  assert(validated_moves_get_validation_status(vms) ==
         MOVE_VALIDATION_STATUS_SUCCESS);
  play_move(validated_moves_get_move(vms, 0), game, NULL);
  validated_moves_destroy(vms);

  game_load_cgp(game, VS_ED);

  assert(!board_get_anchor(board, 3, 3, 0) &&
         !board_get_anchor(board, 3, 3, 1));
  assert(board_get_anchor(board, 12, 12, 0) &&
         board_get_anchor(board, 12, 12, 1));
  assert(!board_get_anchor(board, 4, 3, 0));
  assert(board_get_anchor(board, 4, 3, 1));

  test_board_cross_set_for_cross_set_index(game, 0);
  test_board_cross_set_for_cross_set_index(game, 1);

  // Test with both transpose positions
  for (int i = 0; i < 2; i++) {
    board_reset(board);
    if (i == 1) {
      board_transpose(board);
    }
    board_set_letter(board, 3, 7, 1);
    board_set_letter(board, 0, 10, 2);
    board_set_letter(board, 12, 0, 3);
    board_set_anchor(board, 1, 2, 0, true);
    board_set_anchor(board, 3, 4, 1, true);
    board_set_cross_set(board, 6, 7, 0, 0, 1);
    board_set_cross_set(board, 8, 9, 0, 1, 2);
    board_set_cross_set(board, 10, 11, 1, 0, 3);
    board_set_cross_set(board, 12, 13, 1, 1, 4);
    board_set_cross_score(board, 0, 3, 0, 0, 100);
    board_set_cross_score(board, 1, 4, 0, 1, 200);
    board_set_cross_score(board, 2, 5, 1, 0, 300);
    board_set_cross_score(board, 3, 6, 1, 1, 400);
    board_transpose(board);
    assert(board_get_letter(board, 7, 3) == 1);
    assert(board_get_letter(board, 10, 0) == 2);
    assert(board_get_letter(board, 0, 12) == 3);
    assert(board_get_anchor(board, 2, 1, 1));
    assert(!board_get_anchor(board, 2, 1, 0));
    assert(!board_get_anchor(board, 4, 3, 1));
    assert(board_get_anchor(board, 4, 3, 0));
    assert(board_get_cross_set(board, 7, 6, 1, 0) == 1);
    assert(board_get_cross_set(board, 7, 6, 0, 0) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 7, 6, 0, 1) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 7, 6, 1, 1) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 9, 8, 1, 1) == 2);
    assert(board_get_cross_set(board, 9, 8, 0, 1) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 9, 8, 0, 0) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 9, 8, 1, 0) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 11, 10, 0, 0) == 3);
    assert(board_get_cross_set(board, 11, 10, 1, 0) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 11, 10, 0, 1) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 11, 10, 1, 1) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 13, 12, 0, 1) == 4);
    assert(board_get_cross_set(board, 13, 12, 1, 0) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 13, 12, 1, 1) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_set(board, 13, 12, 0, 0) == TRIVIAL_CROSS_SET);
    assert(board_get_cross_score(board, 3, 0, 1, 0) == 100);
    assert(board_get_cross_score(board, 3, 0, 0, 0) == 0);
    assert(board_get_cross_score(board, 3, 0, 0, 1) == 0);
    assert(board_get_cross_score(board, 3, 0, 1, 1) == 0);
    assert(board_get_cross_score(board, 4, 1, 1, 1) == 200);
    assert(board_get_cross_score(board, 4, 1, 0, 1) == 0);
    assert(board_get_cross_score(board, 4, 1, 0, 0) == 0);
    assert(board_get_cross_score(board, 4, 1, 1, 0) == 0);
    assert(board_get_cross_score(board, 5, 2, 0, 0) == 300);
    assert(board_get_cross_score(board, 5, 2, 1, 0) == 0);
    assert(board_get_cross_score(board, 5, 2, 0, 1) == 0);
    assert(board_get_cross_score(board, 5, 2, 1, 1) == 0);
    assert(board_get_cross_score(board, 6, 3, 0, 1) == 400);
    assert(board_get_cross_score(board, 6, 3, 1, 0) == 0);
    assert(board_get_cross_score(board, 6, 3, 1, 1) == 0);
    assert(board_get_cross_score(board, 6, 3, 0, 0) == 0);
  }

  for (int i = 0; i < 2; i++) {
    test_board_reset(board);
    board_set_letter(board, 10, 4, 23);
    assert(board_get_is_cross_word(board, 9, 4, 0));
    assert(!board_get_is_cross_word(board, 9, 4, 1));
    assert(!board_get_is_cross_word(board, 4, 9, 0));
    assert(!board_get_is_cross_word(board, 4, 9, 1));

    assert(board_get_is_cross_word(board, 11, 4, 0));
    assert(!board_get_is_cross_word(board, 11, 4, 1));
    assert(!board_get_is_cross_word(board, 4, 11, 0));
    assert(!board_get_is_cross_word(board, 4, 11, 1));

    assert(board_get_is_cross_word(board, 10, 3, 1));
    assert(!board_get_is_cross_word(board, 10, 3, 0));
    assert(!board_get_is_cross_word(board, 3, 10, 1));
    assert(!board_get_is_cross_word(board, 3, 10, 0));

    assert(board_get_is_cross_word(board, 10, 5, 1));
    assert(!board_get_is_cross_word(board, 10, 5, 0));
    assert(!board_get_is_cross_word(board, 5, 10, 1));
    assert(!board_get_is_cross_word(board, 5, 10, 0));

    board_transpose(board);
    assert(!board_get_is_cross_word(board, 9, 4, 0));
    assert(!board_get_is_cross_word(board, 9, 4, 1));
    assert(!board_get_is_cross_word(board, 4, 9, 0));
    assert(board_get_is_cross_word(board, 4, 9, 1));

    assert(!board_get_is_cross_word(board, 11, 4, 0));
    assert(!board_get_is_cross_word(board, 11, 4, 1));
    assert(!board_get_is_cross_word(board, 4, 11, 0));
    assert(board_get_is_cross_word(board, 4, 11, 1));

    assert(!board_get_is_cross_word(board, 10, 3, 1));
    assert(!board_get_is_cross_word(board, 10, 3, 0));
    assert(!board_get_is_cross_word(board, 3, 10, 1));
    assert(board_get_is_cross_word(board, 3, 10, 0));

    assert(!board_get_is_cross_word(board, 10, 5, 1));
    assert(!board_get_is_cross_word(board, 10, 5, 0));
    assert(!board_get_is_cross_word(board, 5, 10, 1));
    assert(board_get_is_cross_word(board, 5, 10, 0));
    board_transpose(board);
  }

  assert(!board_get_transposed(board));

  assert(board_are_left_and_right_empty(board, 7, 0));
  assert(board_are_left_and_right_empty(board, 7, BOARD_DIM - 1));
  assert(board_are_left_and_right_empty(board, 7, 3));
  assert(board_are_all_adjacent_squares_empty(board, 7, 0));
  assert(board_are_all_adjacent_squares_empty(board, 7, BOARD_DIM - 1));
  assert(board_are_all_adjacent_squares_empty(board, 7, 3));
  board_set_letter(board, 6, 0, 1);
  board_set_letter(board, 6, BOARD_DIM - 1, 3);
  board_set_letter(board, 6, 3, 3);
  board_set_letter(board, 7, 0, 1);
  board_set_letter(board, 7, BOARD_DIM - 1, 3);
  board_set_letter(board, 7, 3, 3);
  assert(board_are_left_and_right_empty(board, 7, 0));
  assert(board_are_left_and_right_empty(board, 7, BOARD_DIM - 1));
  assert(board_are_left_and_right_empty(board, 7, 3));
  assert(!board_are_all_adjacent_squares_empty(board, 7, 0));
  assert(!board_are_all_adjacent_squares_empty(board, 7, BOARD_DIM));
  assert(!board_are_all_adjacent_squares_empty(board, 7, 3));
  board_set_letter(board, 7, 1, 1);
  board_set_letter(board, 7, BOARD_DIM - 2, 3);
  board_set_letter(board, 7, 4, 3);
  assert(!board_are_left_and_right_empty(board, 7, 3));
  assert(!board_are_left_and_right_empty(board, 7, 0));
  assert(!board_are_left_and_right_empty(board, 7, BOARD_DIM));

  board_set_letter(board, 6, 6, ALPHABET_EMPTY_SQUARE_MARKER);
  board_set_letter(board, 7, 6, ALPHABET_EMPTY_SQUARE_MARKER);
  board_set_letter(board, 8, 6, ALPHABET_EMPTY_SQUARE_MARKER);
  board_set_letter(board, 6, 7, ALPHABET_EMPTY_SQUARE_MARKER);
  board_set_letter(board, 7, 7, ALPHABET_EMPTY_SQUARE_MARKER);
  board_set_letter(board, 8, 7, ALPHABET_EMPTY_SQUARE_MARKER);
  board_set_letter(board, 6, 8, ALPHABET_EMPTY_SQUARE_MARKER);
  board_set_letter(board, 7, 8, ALPHABET_EMPTY_SQUARE_MARKER);
  board_set_letter(board, 8, 8, ALPHABET_EMPTY_SQUARE_MARKER);

  board_set_letter(board, 7, 7, 1);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));

  board_set_letter(board, 7, 8, 1);
  assert(!board_are_all_adjacent_squares_empty(board, 7, 7));
  board_set_letter(board, 7, 8, ALPHABET_EMPTY_SQUARE_MARKER);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));

  board_set_letter(board, 7, 6, 1);
  assert(!board_are_all_adjacent_squares_empty(board, 7, 7));
  board_set_letter(board, 7, 6, ALPHABET_EMPTY_SQUARE_MARKER);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));

  board_set_letter(board, 6, 7, 1);
  assert(!board_are_all_adjacent_squares_empty(board, 7, 7));
  board_set_letter(board, 6, 7, ALPHABET_EMPTY_SQUARE_MARKER);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));

  board_set_letter(board, 8, 7, 1);
  assert(!board_are_all_adjacent_squares_empty(board, 7, 7));
  board_set_letter(board, 8, 7, ALPHABET_EMPTY_SQUARE_MARKER);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));

  board_set_letter(board, 8, 8, 1);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));
  board_set_letter(board, 8, 8, ALPHABET_EMPTY_SQUARE_MARKER);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));

  board_set_letter(board, 6, 8, 1);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));
  board_set_letter(board, 6, 8, ALPHABET_EMPTY_SQUARE_MARKER);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));

  board_set_letter(board, 6, 6, 1);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));
  board_set_letter(board, 6, 6, ALPHABET_EMPTY_SQUARE_MARKER);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));

  board_set_letter(board, 8, 6, 1);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));
  board_set_letter(board, 8, 6, ALPHABET_EMPTY_SQUARE_MARKER);
  assert(board_are_all_adjacent_squares_empty(board, 7, 7));

  assert(!board_get_transposed(board));

  board_set_anchor(board, 1, 2, 0, true);
  assert(board_get_number_of_row_anchors(board, 1, 0) == 1);
  assert(board_get_number_of_row_anchors(board, 2, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 1, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 2, 1) == 0);
  board_set_anchor(board, 3, 4, 0, true);
  assert(board_get_number_of_row_anchors(board, 1, 0) == 1);
  assert(board_get_number_of_row_anchors(board, 2, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 0) == 1);
  assert(board_get_number_of_row_anchors(board, 4, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 1, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 2, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 4, 1) == 0);
  board_set_anchor(board, 1, 4, 0, true);
  assert(board_get_number_of_row_anchors(board, 1, 0) == 2);
  assert(board_get_number_of_row_anchors(board, 2, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 0) == 1);
  assert(board_get_number_of_row_anchors(board, 4, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 1, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 2, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 4, 1) == 0);
  board_set_anchor(board, 3, 2, 0, true);
  assert(board_get_number_of_row_anchors(board, 1, 0) == 2);
  assert(board_get_number_of_row_anchors(board, 2, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 0) == 2);
  assert(board_get_number_of_row_anchors(board, 4, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 1, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 2, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 4, 1) == 0);
  board_set_anchor(board, 9, 12, 1, true);
  assert(board_get_number_of_row_anchors(board, 1, 0) == 2);
  assert(board_get_number_of_row_anchors(board, 2, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 0) == 2);
  assert(board_get_number_of_row_anchors(board, 4, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 9, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 12, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 1, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 2, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 4, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 9, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 12, 1) == 1);
  board_transpose(board);
  board_set_anchor(board, 9, 12, 0, true);
  board_transpose(board);
  assert(board_get_number_of_row_anchors(board, 1, 0) == 2);
  assert(board_get_number_of_row_anchors(board, 2, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 0) == 2);
  assert(board_get_number_of_row_anchors(board, 4, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 9, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 12, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 1, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 2, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 4, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 9, 1) == 1);
  assert(board_get_number_of_row_anchors(board, 12, 1) == 1);
  board_transpose(board);
  board_set_anchor(board, 5, 6, 1, true);
  board_transpose(board);
  assert(board_get_number_of_row_anchors(board, 1, 0) == 2);
  assert(board_get_number_of_row_anchors(board, 2, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 0) == 2);
  assert(board_get_number_of_row_anchors(board, 4, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 5, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 6, 0) == 1);
  assert(board_get_number_of_row_anchors(board, 9, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 12, 0) == 0);
  assert(board_get_number_of_row_anchors(board, 1, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 2, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 3, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 4, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 5, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 6, 1) == 0);
  assert(board_get_number_of_row_anchors(board, 9, 1) == 1);
  assert(board_get_number_of_row_anchors(board, 12, 1) == 1);

  // Test cache load
  int number_of_anchors_cache[BOARD_DIM * 2];
  board_load_number_of_row_anchors_cache(board, number_of_anchors_cache);
  for (int i = 0; i < BOARD_DIM * 2; i++) {
    int expected_number_of_anchors = 0;
    if (i == 1 || i == 3) {
      expected_number_of_anchors = 2;
    } else if (i == 6 || i == 7 || i == 24 || i == 27) {
      expected_number_of_anchors = 1;
    }
    assert(number_of_anchors_cache[i] == expected_number_of_anchors);
  }

  board_transpose(board);
  board_copy(board2, board);

  board_transpose(board);
  board_copy(board2, board);

  assert_boards_are_equal(board, board2);

  test_board_reset(board);

  Square lanes_cache[BOARD_DIM * BOARD_DIM * 2];
  const Square *row_cache;

  board_load_number_of_row_anchors_cache(board, number_of_anchors_cache);
  for (int i = 0; i < BOARD_DIM * 2; i++) {
    int expected_number_of_anchors = 0;
    if (i == 7) {
      expected_number_of_anchors = 1;
    }
    assert(number_of_anchors_cache[i] == expected_number_of_anchors);
  }

  int row_index = 9;
  for (int t = 0; t < 2; t++) {
    if (t == 1) {
      board_transpose(board);
    }
    for (int i = 0; i < BOARD_DIM; i++) {
      board_set_letter(board, row_index, i, i + 2);
      board_set_anchor(board, row_index, i, 0, i % 2 == 0);
      board_set_anchor(board, row_index, i, 1, (i + 1) % 2 == 0);
      board_set_cross_set(board, row_index, i, 0, 0, 50 % (i + 1));
      board_set_cross_set(board, row_index, i, 1, 0, 70 % (i + 1));
    }
    if (t == 1) {
      board_transpose(board);
    }

    board_load_lanes_cache(board, 0, lanes_cache);
    row_cache = board_get_row_cache(lanes_cache, row_index, 0);

    for (int i = 0; i < BOARD_DIM; i++) {
      assert(board_get_letter(board, row_index, i) ==
             square_get_letter(&row_cache[i]));
      assert(board_get_anchor(board, row_index, i, 0) ==
             square_get_anchor(&row_cache[i]));
      assert(board_get_cross_set(board, row_index, i, 0, 0) ==
             square_get_cross_set(&row_cache[i]));
    }
    test_board_reset(board);
  }

  int col_index = 11;
  for (int t = 0; t < 2; t++) {
    if (t == 1) {
      board_transpose(board);
    }
    for (int i = 0; i < BOARD_DIM; i++) {
      board_set_letter(board, i, col_index, i + 2);
      board_set_anchor(board, i, col_index, 0, i % 2 == 0);
      board_set_anchor(board, i, col_index, 1, (i + 1) % 2 == 0);
      board_set_cross_set(board, i, col_index, 0, 0, 53 % (i + 1));
      board_set_cross_set(board, i, col_index, 1, 0, 79 % (i + 1));
    }
    if (t == 1) {
      board_transpose(board);
    }

    board_load_lanes_cache(board, 0, lanes_cache);
    row_cache = board_get_row_cache(lanes_cache, col_index, 1);

    for (int i = 0; i < BOARD_DIM; i++) {
      assert(board_get_letter(board, i, col_index) ==
             square_get_letter(&row_cache[i]));
      assert(board_get_anchor(board, i, col_index, 1) ==
             square_get_anchor(&row_cache[i]));
      assert(board_get_cross_set(board, i, col_index, 1, 0) ==
             square_get_cross_set(&row_cache[i]));
    }
    test_board_reset(board);
  }

  board_destroy(board2);
  game_destroy(game);
  config_destroy(config);
}

void test_board() { test_board_all(); }