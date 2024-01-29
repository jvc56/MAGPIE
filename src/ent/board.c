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
  if (row < BOARD_DIM - 1) {
    tile_below = !board_is_empty(board, row + 1, col);
  }
  if (col < BOARD_DIM - 1) {
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
    for (int i = 0; i < BOARD_DIM; i++) {
      for (int j = 0; j < BOARD_DIM; j++) {
        board_update_anchors(board, i, j, 0);
      }
    }
  } else {
    for (int i = 0; i < BOARD_DIM; i++) {
      for (int j = 0; j < BOARD_DIM; j++) {
        board_reset_anchor(board, i, j, 0);
        board_reset_anchor(board, i, j, 1);
      }
    }
    int rc = BOARD_DIM / 2;
    board_set_anchor(board, rc, rc, 0);
  }
}

void board_reset(Board *board) {
  // The transposed field must be set to 0 here because
  // it is used to calculate the index for board_set_letter.
  board->tiles_played = 0;
  board->transposed = false;

  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      board_set_letter(board, i, j, ALPHABET_EMPTY_SQUARE_MARKER);
    }
  }

  board_set_all_crosses(board);
  board_reset_all_cross_scores(board);
  board_update_all_anchors(board);
}

void board_set_bonus_squares(Board *board) {
  for (int i = 0; i < BOARD_DIM * BOARD_DIM; i++) {
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

Board *board_create() {
  Board *board = malloc_or_die(sizeof(Board));
  board_reset(board);
  board_set_bonus_squares(board);
  return board;
}

Board *board_duplicate(const Board *board) {
  Board *new_board = malloc_or_die(sizeof(Board));
  board_copy(new_board, board);
  board_set_bonus_squares(new_board);
  return new_board;
}

// Copies the letters, cross sets, anchors,
// transposed state, and number of tiles played
// from src to dst. Does not copy bonus squares
// since it is assumed that src and dst have the
// same bonus squares.
void board_copy(Board *dst, const Board *src) {
  // Transposed must be set before copying
  // since the get and set methods use board_transpose
  // for access.
  dst->transposed = src->transposed;
  dst->tiles_played = src->tiles_played;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
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

void board_destroy(Board *board) {
  if (!board) {
    return;
  }
  free(board);
}

// this function assumes the word is always horizontal. If this isn't the case,
// the board needs to be transposed ahead of time.
int board_score_move(const Board *board, const LetterDistribution *ld,
                     uint8_t word[], int word_start_index, int word_end_index,
                     int row, int col, int tiles_played, int cross_dir,
                     int cross_set_index) {
  int ls;
  int main_word_score = 0;
  int cross_scores = 0;
  int bingo_bonus = 0;
  if (tiles_played == RACK_SIZE) {
    bingo_bonus = DEFAULT_BINGO_BONUS;
  }
  int word_multiplier = 1;
  for (int idx = 0; idx < word_end_index - word_start_index + 1; idx++) {
    uint8_t ml = word[idx + word_start_index];
    uint8_t bonus_square = board_get_bonus_square(board, row, col + idx);
    int letter_multiplier = 1;
    int this_word_multiplier = 1;
    bool fresh_tile = false;
    if (ml == PLAYED_THROUGH_MARKER) {
      ml = board_get_letter(board, row, col + idx);
    } else {
      fresh_tile = true;
      this_word_multiplier = bonus_square >> 4;
      letter_multiplier = bonus_square & 0x0F;
      word_multiplier *= this_word_multiplier;
    }
    int cs = board_get_cross_score(board, row, col + idx, cross_dir,
                                   cross_set_index);
    if (get_is_blanked(ml)) {
      ls = 0;
    } else {
      ls = ld_get_score(ld, ml);
    }

    main_word_score += ls * letter_multiplier;
    bool actual_cross_word =
        (row > 0 && !board_is_empty(board, row - 1, col + idx)) ||
        ((row < BOARD_DIM - 1) && !board_is_empty(board, row + 1, col + idx));
    if (fresh_tile && actual_cross_word) {
      cross_scores += ls * letter_multiplier * this_word_multiplier +
                      cs * this_word_multiplier;
    }
  }
  return main_word_score * word_multiplier + cross_scores + bingo_bonus;
}
