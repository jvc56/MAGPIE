#include "board.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"

#include "../util/string_util.h"
#include "../util/util.h"

board_layout_t
board_layout_string_to_board_layout(const char *board_layout_string) {
  if (strings_equal(board_layout_string, BOARD_LAYOUT_CROSSWORD_GAME_NAME)) {
    return BOARD_LAYOUT_CROSSWORD_GAME;
  }
  if (strings_equal(board_layout_string,
                    BOARD_LAYOUT_SUPER_CROSSWORD_GAME_NAME)) {
    return BOARD_LAYOUT_SUPER_CROSSWORD_GAME;
  }
  return BOARD_LAYOUT_UNKNOWN;
}

void board_update_anchors(Board *board, int row, int col, int dir) {
  if (board_is_dir_vertical(dir)) {
    int temp = row;
    row = col;
    col = temp;
  }

  board_reset_anchor(board, row, col, 0);
  board_reset_anchor(board, row, col, 1);
  bool tile_above = false;
  bool tile_below = false;
  bool tile_left = false;
  bool tile_right = false;
  bool tile_here = false;

  if (row > 0) {
    tile_above = !board_is_empty(board, row - 1, col);
  }
  if (col > 0) {
    tile_left = !board_is_empty(board, row, col - 1);
  }
  if (row < board->number_of_rows - 1) {
    tile_below = !board_is_empty(board, row + 1, col);
  }
  if (col < board->number_of_cols - 1) {
    tile_right = !board_is_empty(board, row, col + 1);
  }
  tile_here = !board_is_empty(board, row, col);
  if (tile_here) {
    if (!tile_right) {
      board_set_anchor(board, row, col, 0);
    }
    if (!tile_below) {
      board_set_anchor(board, row, col, 1);
    }
  } else {
    if (!tile_left && !tile_right && (tile_above || tile_below)) {
      board_set_anchor(board, row, col, 0);
    }
    if (!tile_above && !tile_below && (tile_left || tile_right)) {
      board_set_anchor(board, row, col, 1);
    }
  }
}

void board_update_all_anchors(Board *board) {
  if (board->tiles_played > 0) {
    for (int i = 0; i < board->number_of_rows; i++) {
      for (int j = 0; j < board->number_of_cols; j++) {
        board_update_anchors(board, i, j, 0);
      }
    }
  } else {
    for (int i = 0; i < board->number_of_rows; i++) {
      for (int j = 0; j < board->number_of_cols; j++) {
        board_reset_anchor(board, i, j, 0);
        board_reset_anchor(board, i, j, 1);
      }
    }
    // FIXME: In later updates we will get the
    // start square from input args.
    board_set_anchor(board, board->number_of_rows / 2,
                     board->number_of_cols / 2, 0);
  }
}

void board_reset(Board *board) {
  // The transposed field must be set to 0 here because
  // it is used to calculate the index for board_set_letter.
  board->tiles_played = 0;
  board->transposed = false;

  for (int i = 0; i < board->number_of_rows; i++) {
    for (int j = 0; j < board->number_of_cols; j++) {
      board_set_letter(board, i, j, ALPHABET_EMPTY_SQUARE_MARKER);
    }
  }

  board_set_all_crosses(board);
  board_reset_all_cross_scores(board);
  board_update_all_anchors(board);
}

void board_set_bonus_squares(Board *board) {
  // FIXME: this will need to be updated
  // to take arguments to set bonus squares
  // instead of constants.
  for (int i = 0; i < board->number_of_rows * board->number_of_cols; i++) {
    uint8_t bonus_value;
    char bonus_square = CROSSWORD_GAME_BOARD[i];
    if (bonus_square == BONUS_TRIPLE_WORD_SCORE) {
      bonus_value = 3;
      bonus_value = bonus_value << 4;
      bonus_value += 1;
    } else if (bonus_square == BONUS_DOUBLE_WORD_SCORE) {
      bonus_value = 2;
      bonus_value = bonus_value << 4;
      bonus_value += 1;
    } else if (bonus_square == BONUS_DOUBLE_LETTER_SCORE) {
      bonus_value = 1;
      bonus_value = bonus_value << 4;
      bonus_value += 2;
    } else if (bonus_square == BONUS_TRIPLE_LETTER_SCORE) {
      bonus_value = 1;
      bonus_value = bonus_value << 4;
      bonus_value += 3;
    } else {
      bonus_value = 1;
      bonus_value = bonus_value << 4;
      bonus_value += 1;
    }
    board->bonus_squares[i] = bonus_value;
  }
}

// Assumes the number_of_rows and number_of_cols has been set.
void board_alloc(Board *board, int number_of_rows, int number_of_cols) {
  board->number_of_rows = number_of_rows;
  board->number_of_cols = number_of_cols;
  int area = board_get_area(board);
  // See the .h for more details on these allocations
  board->letters = malloc_or_die(sizeof(uint8_t) * area);
  board->bonus_squares = malloc_or_die(sizeof(uint8_t) * area);
  board->cross_sets = malloc_or_die(sizeof(uint64_t) * area * 4);
  board->cross_scores = malloc_or_die(sizeof(int) * area * 4);
  board->anchors = malloc_or_die(sizeof(bool) * area * 2);
}

Board *board_create() {
  Board *board = malloc_or_die(sizeof(Board));

  // FIXME: Hard code these values to 15 for now. In later
  // updates height and width will be given as arguments.
  board_alloc(board, 15, 15);

  board_reset(board);
  board_set_bonus_squares(board);
  return board;
}

void board_destroy(Board *board) {
  if (!board) {
    return;
  }

  free(board->letters);
  free(board->bonus_squares);
  free(board->cross_sets);
  free(board->cross_scores);
  free(board->anchors);

  free(board);
}

Board *board_duplicate(const Board *board) {
  Board *new_board = malloc_or_die(sizeof(Board));
  board_alloc(new_board, board_get_number_of_rows(board),
              board_get_number_of_cols(board));
  board_copy(new_board, board);
  board_set_bonus_squares(new_board);
  return new_board;
}

// Copies the letters, cross sets, anchors,
// transposed state, and number of tiles played
// from src to dst. Does not copy bonus squares
// since it is assumed that src and dst have the
// same bonus squares.
// Assumes that the boards have the same dimensions.
void board_copy(Board *dst, const Board *src) {
  // Transposed must be set before copying
  // since the get and set methods use board_transpose
  // for access.
  dst->transposed = src->transposed;
  dst->tiles_played = src->tiles_played;
  for (int row = 0; row < src->number_of_rows; row++) {
    for (int col = 0; col < src->number_of_cols; col++) {
      board_set_letter(dst, row, col, board_get_letter(src, row, col));
      for (int dir = 0; dir < 2; dir++) {
        if (board_get_anchor(src, row, col, dir)) {
          board_set_anchor(dst, row, col, dir);
        } else {
          board_reset_anchor(dst, row, col, dir);
        }
        for (int cross_set_index = 0; cross_set_index < 2; cross_set_index++) {
          board_set_cross_set(
              dst, row, col,
              board_get_cross_set(src, row, col, dir, cross_set_index), dir,
              cross_set_index);
          board_set_cross_score(
              dst, row, col,
              board_get_cross_score(src, row, col, dir, cross_set_index), dir,
              cross_set_index);
        }
      }
    }
  }
}
