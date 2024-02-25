#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"

#include "letter_distribution.h"

#include "../util/string_util.h"
#include "../util/util.h"

typedef struct Board {
  uint8_t letters[BOARD_DIM * BOARD_DIM];
  // Bonus squares are set at board creation
  // and should not be modified.
  uint8_t bonus_squares[BOARD_DIM * BOARD_DIM];
  // We use (number of squares * 4) for cross sets
  // to account for
  //   - vertical and horizontal board directions
  //   - separate lexicons used by player 1 and player 2
  uint64_t cross_sets[BOARD_DIM * BOARD_DIM * 4];
  // We use (number of squares * 4) for cross scores
  // for reasons listed above.
  int cross_scores[BOARD_DIM * BOARD_DIM * 4];
  // We use (number of squares * 2) for cross sets
  // to account for
  //   - vertical and horizontal board directions
  bool anchors[BOARD_DIM * BOARD_DIM * 2];
  bool transposed;
  int tiles_played;
  // Scratch pad for return values used by
  // traverse backwards for score
  uint32_t node_index;
  bool path_is_valid;
} Board;

// Current index
// depends on tranposition of the board

static inline int board_get_tindex(const Board *board, int row, int col) {
  if (!board->transposed) {
    return (row * BOARD_DIM) + col;
  } else {
    return (col * BOARD_DIM) + row;
  }
}

static inline int board_get_tindex_dir(const Board *board, int row, int col,
                                       int dir) {
  return board_get_tindex(board, row, col) * 2 + dir;
}

static inline int board_get_tindex_player_cross(const Board *board, int row,
                                                int col, int dir,
                                                int cross_set_index) {
  return board_get_tindex_dir(board, row, col, dir) +
         (BOARD_DIM * BOARD_DIM * 2) * cross_set_index;
}

static inline uint8_t board_get_letter(const Board *board, int row, int col) {
  return board->letters[board_get_tindex(board, row, col)];
}

static inline bool board_is_empty(const Board *board, int row, int col) {
  return board_get_letter(board, row, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

static inline bool board_is_letter_allowed_in_cross_set(uint64_t cross_set,
                                                        uint8_t letter) {
  return (cross_set & ((uint64_t)1 << letter)) != 0;
}

static inline bool board_is_dir_vertical(int dir) {
  return dir == BOARD_VERTICAL_DIRECTION;
}

// Anchors

static inline bool board_get_anchor(const Board *board, int row, int col,
                                    int dir) {
  return board->anchors[board_get_tindex_dir(board, row, col, dir)];
}

static inline void board_set_anchor(Board *board, int row, int col, int dir) {
  board->anchors[board_get_tindex_dir(board, row, col, dir)] = true;
}

static inline void board_reset_anchor(Board *board, int row, int col, int dir) {
  board->anchors[board_get_tindex_dir(board, row, col, dir)] = false;
}

// Cross sets and scores

static inline uint64_t *board_get_cross_set_pointer(Board *board, int row,
                                                    int col, int dir,
                                                    int cross_set_index) {
  return &board->cross_sets[board_get_tindex_player_cross(board, row, col, dir,
                                                          cross_set_index)];
}

static inline uint64_t board_get_cross_set(const Board *board, int row, int col,
                                           int dir, int cross_set_index) {
  return board->cross_sets[board_get_tindex_player_cross(board, row, col, dir,
                                                         cross_set_index)];
}

static inline void board_set_cross_score(Board *board, int row, int col,
                                         int score, int dir,
                                         int cross_set_index) {
  board->cross_scores[board_get_tindex_player_cross(board, row, col, dir,
                                                    cross_set_index)] = score;
}

static inline int board_get_cross_score(const Board *board, int row, int col,
                                        int dir, int cross_set_index) {
  return board->cross_scores[board_get_tindex_player_cross(board, row, col, dir,
                                                           cross_set_index)];
}

static inline uint8_t board_get_bonus_square(const Board *board, int row,
                                             int col) {
  return board->bonus_squares[board_get_tindex(board, row, col)];
}

static inline void board_set_cross_set_letter(uint64_t *cross_set,
                                              uint8_t letter) {
  *cross_set = *cross_set | ((uint64_t)1 << letter);
}

static inline void board_set_cross_set(Board *board, int row, int col,
                                       uint64_t letter, int dir,
                                       int cross_set_index) {
  board->cross_sets[board_get_tindex_player_cross(board, row, col, dir,
                                                  cross_set_index)] = letter;
}

static inline void board_clear_cross_set(Board *board, int row, int col,
                                         int dir, int cross_set_index) {
  board->cross_sets[board_get_tindex_player_cross(board, row, col, dir,
                                                  cross_set_index)] = 0;
}

static inline void board_set_all_crosses(Board *board) {
  for (int i = 0; i < BOARD_DIM * BOARD_DIM * 2 * 2; i++) {
    board->cross_sets[i] = TRIVIAL_CROSS_SET;
  }
}

static inline void board_reset_all_cross_scores(Board *board) {
  for (size_t i = 0; i < (BOARD_DIM * BOARD_DIM * 2 * 2); i++) {
    board->cross_scores[i] = 0;
  }
}

static inline bool board_is_position_valid(int row, int col) {
  return row >= 0 && row < BOARD_DIM && col >= 0 && col < BOARD_DIM;
}

static inline bool board_are_left_and_right_empty(const Board *board, int row,
                                                  int col) {
  return !((board_is_position_valid(row, col - 1) &&
            !board_is_empty(board, row, col - 1)) ||
           (board_is_position_valid(row, col + 1) &&
            !board_is_empty(board, row, col + 1)));
}

static inline bool board_are_all_adjacent_squares_empty(const Board *board,
                                                        int row, int col) {
  return !((board_is_position_valid(row, col - 1) &&
            !board_is_empty(board, row, col - 1)) ||
           (board_is_position_valid(row, col + 1) &&
            !board_is_empty(board, row, col + 1)) ||
           (board_is_position_valid(row - 1, col) &&
            !board_is_empty(board, row - 1, col)) ||
           (board_is_position_valid(row + 1, col) &&
            !board_is_empty(board, row + 1, col)));
}

static inline int board_get_word_edge(const Board *board, int row, int col,
                                      int dir) {
  while (board_is_position_valid(row, col) &&
         !board_is_empty(board, row, col)) {
    col += dir;
  }
  return col - dir;
}

static inline int board_get_cross_set_index(bool kwgs_are_shared,
                                            int player_index) {
  return (!kwgs_are_shared) && player_index;
}

static inline void board_set_letter(Board *board, int row, int col,
                                    uint8_t letter) {
  board->letters[board_get_tindex(board, row, col)] = letter;
}

static inline bool board_get_transposed(const Board *board) {
  return board->transposed;
}

static inline void board_transpose(Board *board) {
  board->transposed = !board->transposed;
}

static inline void board_set_transposed(Board *board, bool transposed) {
  board->transposed = transposed;
}

static inline bool board_matches_dir(const Board *board, int dir) {
  return (board_is_dir_vertical(dir) && board_get_transposed(board)) ||
         (!board_is_dir_vertical(dir) && !board_get_transposed(board));
}

static inline int board_get_tiles_played(const Board *board) {
  return board->tiles_played;
}

static inline bool board_get_path_is_valid(const Board *board) {
  return board->path_is_valid;
}

static inline uint32_t board_get_node_index(const Board *board) {
  return board->node_index;
}

static inline void board_set_node_index(Board *board, uint32_t value) {
  board->node_index = value;
}

static inline void board_set_path_is_valid(Board *board, bool value) {
  board->path_is_valid = value;
}

static inline void board_increment_tiles_played(Board *board,
                                                int tiles_played) {
  board->tiles_played += tiles_played;
}

static inline board_layout_t
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

static inline void board_update_anchors(Board *board, int row, int col,
                                        int dir) {
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

static inline void board_update_all_anchors(Board *board) {
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

static inline void board_reset(Board *board) {
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

static inline void board_set_bonus_squares(Board *board) {
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

static inline Board *board_create() {
  Board *board = malloc_or_die(sizeof(Board));
  board_reset(board);
  board_set_bonus_squares(board);
  return board;
}

// Copies the letters, cross sets, anchors,
// transposed state, and number of tiles played
// from src to dst. Does not copy bonus squares
// since it is assumed that src and dst have the
// same bonus squares.
static inline void board_copy(Board *dst, const Board *src) {
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

static inline Board *board_duplicate(const Board *board) {
  Board *new_board = malloc_or_die(sizeof(Board));
  board_copy(new_board, board);
  board_set_bonus_squares(new_board);
  return new_board;
}

static inline void board_destroy(Board *board) {
  if (!board) {
    return;
  }
  free(board);
}
#endif