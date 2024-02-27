#include <assert.h>
#include <stdint.h>

#include "../../src/def/board_defs.h"

#include "../../src/ent/board.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"

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
}

void test_board_reset(Board *board) {
  board_reset(board);
  for (int grid_index = 0; grid_index < 2; grid_index++) {
    const Grid *g = board_get_const_grid(board, grid_index);
    for (int row = 0; row < BOARD_DIM; row++) {
      assert(grid_get_anchors_at_row(g, row) == (row == BOARD_DIM / 2));
      for (int col = 0; col < BOARD_DIM; col++) {
        assert(board_get_letter(board, row, col) ==
               ALPHABET_EMPTY_SQUARE_MARKER);
        for (int dir = 0; dir < 2; dir++) {
          if (row == (BOARD_DIM / 2) && col == (BOARD_DIM / 2) &&
              grid_index == dir) {
            assert(board_get_anchor(board, row, col, dir));
          } else {
            assert(!board_get_anchor(board, row, col, dir));
          }
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

void print_board(Board *board) {
  for (int grid_index = 0; grid_index < 2; grid_index++) {
    const Grid *g = board_get_const_grid(board, grid_index);
    printf("GRID %d\n", grid_index);
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        const Square *s = grid_get_const_square(g, row, col);
        printf("(%d, %d): %d, %d, %d, %d\n", row, col, square_get_letter(s),
               square_get_bonus_square(s), square_get_anchor(s, 0),
               square_get_anchor(s, 1));
      }
    }
  }
}

void test_board_everything() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Board *board = game_get_board(game);
  Board *board2 = board_duplicate(board);

  game_load_cgp(game, VS_ED);

  assert(!board_get_anchor(board, 3, 3, 0) &&
         !board_get_anchor(board, 3, 3, 1));
  assert(board_get_anchor(board, 12, 12, 0) &&
         board_get_anchor(board, 12, 12, 1));
  assert(board_get_anchor(board, 4, 3, 1) && !board_get_anchor(board, 4, 3, 0));

  test_board_cross_set_for_cross_set_index(game, 0);
  test_board_cross_set_for_cross_set_index(game, 1);

  // Test with both transpose positions
  for (int i = 0; i < 2; i++) {
    board_reset(board);
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

  test_board_reset(board);

  // Test is_cross_word
  board_set_letter(board, 10, 4, 1);
  board_set_letter(board, 0, 7, 1);
  assert(!board_get_is_cross_word(board, 10, 5));
  assert(!board_get_is_cross_word(board, 0, 8));
  assert(!board_get_is_cross_word(board, 3, 0));
  board_transpose(board);
  assert(board_get_is_cross_word(board, 5, 10));
  assert(board_get_is_cross_word(board, 8, 0));
  assert(!board_get_is_cross_word(board, 0, 3));
  board_transpose(board);
  board_set_letter(board, 9, 5, 1);
  board_set_letter(board, 2, 0, 1);
  assert(board_get_is_cross_word(board, 10, 5));
  assert(!board_get_is_cross_word(board, 0, 8));
  assert(board_get_is_cross_word(board, 3, 0));
  board_transpose(board);
  assert(board_get_is_cross_word(board, 5, 10));
  assert(board_get_is_cross_word(board, 8, 0));
  assert(!board_get_is_cross_word(board, 0, 3));
  board_transpose(board);
  board_set_letter(board, 11, 5, 1);
  board_set_letter(board, 4, 0, 1);
  board_set_letter(board, 1, 8, 1);
  assert(board_get_is_cross_word(board, 10, 5));
  assert(board_get_is_cross_word(board, 0, 8));
  assert(board_get_is_cross_word(board, 3, 0));
  board_transpose(board);
  assert(board_get_is_cross_word(board, 5, 10));
  assert(board_get_is_cross_word(board, 8, 0));
  assert(!board_get_is_cross_word(board, 0, 3));
  board_transpose(board);
  board_set_letter(board, 10, 6, 1);
  board_set_letter(board, 3, 1, 1);
  board_set_letter(board, 0, 9, 1);
  assert(board_get_is_cross_word(board, 10, 5));
  assert(board_get_is_cross_word(board, 0, 8));
  assert(board_get_is_cross_word(board, 3, 0));
  board_transpose(board);
  assert(board_get_is_cross_word(board, 5, 10));
  assert(board_get_is_cross_word(board, 8, 0));
  assert(board_get_is_cross_word(board, 0, 3));
  board_transpose(board);

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
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 1) == 1);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 1) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 2) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 2) == 1);
  board_set_anchor(board, 3, 4, 0, true);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 1) == 1);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 1) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 2) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 2) == 1);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 3) == 1);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 3) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 4) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 4) == 1);
  board_set_anchor(board, 1, 4, 0, true);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 1) == 2);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 1) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 2) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 2) == 1);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 3) == 1);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 3) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 4) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 4) == 2);
  board_set_anchor(board, 3, 2, 0, true);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 1) == 2);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 1) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 2) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 2) == 2);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 3) == 2);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 3) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 4) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 4) == 2);

  // Test cache load
  int number_of_anchors_cache[BOARD_DIM * 2];
  board_load_number_of_row_anchors_cache(board, number_of_anchors_cache);
  for (int i = 0; i < BOARD_DIM * 2; i++) {
    int expected_number_of_anchors = 0;
    if (i == 1 || i == 3 || i == 17 || i == 19) {
      expected_number_of_anchors = 2;
    } else if (i == 7 || i == 22) {
      expected_number_of_anchors = 1;
    }
    assert(number_of_anchors_cache[i] == expected_number_of_anchors);
  }

  board_set_anchor(board, 3, 4, 0, false);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 1) == 2);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 1) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 2) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 2) == 2);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 3) == 1);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 3) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 4) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 4) == 1);
  board_set_anchor(board, 3, 2, 0, false);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 1) == 2);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 1) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 2) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 2) == 1);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 3) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 3) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 4) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 4) == 1);
  board_set_anchor(board, 1, 2, 0, false);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 1) == 1);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 1) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 2) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 2) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 3) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 3) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 4) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 4) == 1);
  board_set_anchor(board, 1, 4, 0, false);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 1) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 1) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 2) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 2) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 3) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 3) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 0), 4) == 0);
  assert(grid_get_anchors_at_row(board_get_const_grid(board, 1), 4) == 0);

  board_copy(board2, board);
  assert_boards_are_equal(board, board2);

  test_board_reset(board);

  Square squares[BOARD_DIM];

  board_load_number_of_row_anchors_cache(board, number_of_anchors_cache);
  for (int i = 0; i < BOARD_DIM * 2; i++) {
    int expected_number_of_anchors = 0;
    if (i == 7 || i == 22) {
      expected_number_of_anchors = 1;
    }
    assert(number_of_anchors_cache[i] == expected_number_of_anchors);
  }

  int row_index = 9;
  for (int i = 0; i < BOARD_DIM; i++) {
    board_set_letter(board, row_index, i, i + 2);
    board_set_anchor(board, row_index, i, 0, i % 2);
    board_set_anchor(board, row_index, i, 1, i % 3);
    board_set_cross_set(board, row_index, i, 0, 0, 5);
    board_set_cross_set(board, row_index, i, 1, 0, 7);
  }

  board_load_row_cache(board, row_index, 0, squares);

  for (int i = 0; i < BOARD_DIM; i++) {
    assert(board_get_letter(board, row_index, i) ==
           square_get_letter(&squares[i]));
    assert(board_get_anchor(board, row_index, i, 0) ==
           square_get_anchor(&squares[i], 0));
    assert(board_get_anchor(board, row_index, i, 1) ==
           square_get_anchor(&squares[i], 1));
    assert(board_get_cross_set(board, row_index, i, 0, 0) ==
           square_get_cross_set(&squares[i], 0, 0));
    assert(board_get_cross_set(board, row_index, i, 1, 0) ==
           square_get_cross_set(&squares[i], 1, 0));
  }

  test_board_reset(board);

  int col_index = 11;
  for (int i = 0; i < BOARD_DIM; i++) {
    board_set_letter(board, i, col_index, i + 2);
    board_set_anchor(board, i, col_index, 0, i % 2);
    board_set_anchor(board, i, col_index, 1, i % 3);
    board_set_cross_set(board, i, col_index, 0, 0, 5);
    board_set_cross_set(board, i, col_index, 1, 0, 7);
  }

  board_load_row_cache(board, col_index, 1, squares);

  for (int i = 0; i < BOARD_DIM; i++) {
    assert(board_get_letter(board, i, col_index) ==
           square_get_letter(&squares[i]));
    // Direction gets flipped
    assert(board_get_anchor(board, i, col_index, 0) ==
           square_get_anchor(&squares[i], 1));
    assert(board_get_anchor(board, i, col_index, 1) ==
           square_get_anchor(&squares[i], 0));
    assert(board_get_cross_set(board, i, col_index, 0, 0) ==
           square_get_cross_set(&squares[i], 1, 0));
    assert(board_get_cross_set(board, i, col_index, 1, 0) ==
           square_get_cross_set(&squares[i], 0, 0));
  }

  test_board_reset(board);

  board_destroy(board2);
  game_destroy(game);
  config_destroy(config);
}

void test_board() { test_board_everything(); }