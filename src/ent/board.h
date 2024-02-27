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

typedef struct Square {
  uint8_t letter;
  // Bonus squares are set at board creation
  // and should not be modified.
  uint8_t bonus_square;
  // We use (number of squares * 4) for cross sets
  // to account for
  //   - vertical and horizontal board directions
  //   - separate lexicons used by player 1 and player 2
  uint64_t cross_sets[4];
  // We use (number of squares * 4) for cross scores
  // for reasons listed above.
  int cross_scores[4];
  // We use (number of squares * 2) for cross sets
  // to account for
  //   - vertical and horizontal board directions
  bool anchors[2];
  // Nonzero if placing a tile on this square would
  // form a cross word
  int is_cross_word;
} Square;

typedef struct Grid {
  Square squares[BOARD_DIM * BOARD_DIM];
  int number_of_row_anchors[BOARD_DIM];
} Grid;

// Board maintains two grids, the grid at 1
// being a transpose of the grid at 0. Accesses
// to grid are control by the transposed state
// and write are applied to both grids.
typedef struct Board {
  Grid grids[2];
  int transposed;
  int tiles_played;
  // Scratch pad for return values used by
  // traverse backwards for score
  uint32_t node_index;
  bool path_is_valid;
} Board;

// Square: Index helpers

static inline int square_get_dir_cross_index(int dir, int cross_index) {
  return 2 * cross_index + dir;
}

static inline int square_get_dir_index(int dir) { return dir; }

// Square: Letter

static inline uint8_t square_get_letter(const Square *s) { return s->letter; }

static inline void square_set_letter(Square *s, uint8_t letter) {
  s->letter = letter;
}

// Square: Bonus square

static inline uint8_t square_get_bonus_square(const Square *s) {
  return s->bonus_square;
}

static inline void square_set_bonus_square(Square *s, uint8_t bonus_square) {
  s->bonus_square = bonus_square;
}

// Square: Cross sets

static inline uint64_t square_get_cross_set(const Square *s, int dir,
                                            int cross_index) {
  return s->cross_sets[square_get_dir_cross_index(dir, cross_index)];
}

static inline void square_set_cross_set(Square *s, int dir, int cross_index,
                                        uint64_t cross_set) {
  s->cross_sets[square_get_dir_cross_index(dir, cross_index)] = cross_set;
}

static inline uint64_t get_cross_set_bit(uint8_t letter) {
  return (uint64_t)1 << letter;
}

static inline void square_set_cross_set_letter(Square *s, int dir,
                                               int cross_index,
                                               uint8_t letter) {
  int dir_cross_index = square_get_dir_cross_index(dir, cross_index);
  s->cross_sets[dir_cross_index] |= get_cross_set_bit(letter);
}

// Square: Cross scores

static inline int square_get_cross_score(const Square *s, int dir,
                                         int cross_index) {
  return s->cross_scores[square_get_dir_cross_index(dir, cross_index)];
}

static inline void square_set_cross_score(Square *s, int dir, int cross_index,
                                          int score) {
  s->cross_scores[square_get_dir_cross_index(dir, cross_index)] = score;
}

// Square: Anchors

static inline bool square_get_anchor(const Square *s, int dir) {
  return s->anchors[square_get_dir_index(dir)];
}

static inline void square_set_anchor(Square *s, int dir, bool anchor) {
  s->anchors[square_get_dir_index(dir)] = anchor;
}

// Square: is cross word

static inline bool square_get_is_cross_word(const Square *s) {
  return s->is_cross_word != 0;
}

static inline void square_increment_is_cross_word(Square *s) {
  s->is_cross_word++;
}

static inline void square_reset_is_cross_word(Square *s) {
  s->is_cross_word = 0;
}

// Grid: Index helpers

static inline int grid_get_index(int row, int col) {
  return (row * BOARD_DIM) + col;
}

static inline const Square *grid_get_const_square(const Grid *g, int row,
                                                  int col) {
  return &g->squares[grid_get_index(row, col)];
}

static inline Square *grid_get_mutable_square(Grid *g, int row, int col) {
  return &g->squares[grid_get_index(row, col)];
}

// Grid: Letter

static inline uint8_t grid_get_letter(const Grid *g, int row, int col) {
  return square_get_letter(grid_get_const_square(g, row, col));
}

// Precondition: the square is empty
static inline void grid_set_letter(Grid *g, int row, int col, uint8_t letter) {
  square_set_letter(grid_get_mutable_square(g, row, col), letter);
  if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    if (row > 0) {
      square_increment_is_cross_word(grid_get_mutable_square(g, row - 1, col));
    }
    if (row < BOARD_DIM - 1) {
      square_increment_is_cross_word(grid_get_mutable_square(g, row + 1, col));
    }
  }
}

// Grid: Bonus square

static inline uint8_t grid_get_bonus_square(const Grid *g, int row, int col) {
  return square_get_bonus_square(grid_get_const_square(g, row, col));
}

static inline void grid_set_bonus_square(Grid *g, int row, int col,
                                         uint8_t bonus_square) {
  square_set_bonus_square(grid_get_mutable_square(g, row, col), bonus_square);
}

// Grid: Cross sets

static inline uint64_t grid_get_cross_set(const Grid *g, int row, int col,
                                          int dir, int cross_index) {
  return square_get_cross_set(grid_get_const_square(g, row, col), dir,
                              cross_index);
}

static inline void grid_set_cross_set(Grid *g, int row, int col, int dir,
                                      int cross_index, uint64_t cross_set) {
  square_set_cross_set(grid_get_mutable_square(g, row, col), dir, cross_index,
                       cross_set);
}

static inline void grid_set_cross_set_letter(Grid *g, int row, int col, int dir,
                                             int cross_index, uint8_t letter) {
  square_set_cross_set_letter(grid_get_mutable_square(g, row, col), dir,
                              cross_index, letter);
}

// Grid: Cross scores

static inline int grid_get_cross_score(const Grid *g, int row, int col, int dir,
                                       int cross_index) {
  return square_get_cross_score(grid_get_const_square(g, row, col), dir,
                                cross_index);
}

static inline void grid_set_cross_score(Grid *g, int row, int col, int dir,
                                        int cross_index, int cross_score) {
  square_set_cross_score(grid_get_mutable_square(g, row, col), dir, cross_index,
                         cross_score);
}

// Grid: Anchors

static inline bool grid_get_anchor(const Grid *g, int row, int col, int dir) {
  return square_get_anchor(grid_get_const_square(g, row, col), dir);
}

static inline void grid_set_anchor(Grid *g, int row, int col, int dir,
                                   bool anchor) {
  Square *s = grid_get_mutable_square(g, row, col);
  bool current_anchor = square_get_anchor(s, dir);
  // FIXME: Maybe use this instead?
  // g->number_of_row_anchors[row] += (int)anchor - (int)current_anchor;
  // The compiler should make short work of this anyway
  if (current_anchor && !anchor) {
    g->number_of_row_anchors[row]--;
  } else if (!current_anchor && anchor) {
    g->number_of_row_anchors[row]++;
  }
  square_set_anchor(s, dir, anchor);
}

// This bypasses the modification of the number of row anchors and
// should only be used when resetting the grid.
static inline void grid_reset_anchor(Grid *g, int row, int col, int dir) {
  square_set_anchor(grid_get_mutable_square(g, row, col), dir, false);
}

static inline int grid_get_anchors_at_row(const Grid *g, int row) {
  return g->number_of_row_anchors[row];
}

// Grid: is cross word

static inline bool grid_get_is_cross_word(const Grid *g, int row, int col) {
  return square_get_is_cross_word(grid_get_const_square(g, row, col));
}

static inline void grid_reset_is_cross_word(Grid *g, int row, int col) {
  square_reset_is_cross_word(grid_get_mutable_square(g, row, col));
}

// Board: Index helpers

static inline const Grid *board_get_const_grid(const Board *b, int grid_index) {
  return &b->grids[grid_index];
}

static inline Grid *board_get_mutable_grid(Board *b, int grid_index) {
  return &b->grids[grid_index];
}

// Board: Letter

static inline uint8_t board_get_letter(const Board *b, int row, int col) {
  return grid_get_letter(board_get_const_grid(b, b->transposed), row, col);
}

static inline void board_set_letter(Board *b, int row, int col,
                                    uint8_t letter) {
  grid_set_letter(board_get_mutable_grid(b, b->transposed), row, col, letter);
  grid_set_letter(board_get_mutable_grid(b, 1 - b->transposed), col, row,
                  letter);
}

// Board: Bonus square

static inline uint8_t board_get_bonus_square(const Board *b, int row, int col) {
  return grid_get_bonus_square(board_get_const_grid(b, b->transposed), row,
                               col);
}

// Board: Cross set

static inline uint64_t board_get_cross_set(const Board *b, int row, int col,
                                           int dir, int cross_index) {
  return grid_get_cross_set(board_get_const_grid(b, b->transposed), row, col,
                            dir, cross_index);
}

static inline void board_set_cross_set(Board *b, int row, int col, int dir,
                                       int cross_index, uint64_t cross_set) {
  grid_set_cross_set(board_get_mutable_grid(b, b->transposed), row, col, dir,
                     cross_index, cross_set);
  grid_set_cross_set(board_get_mutable_grid(b, 1 - b->transposed), col, row,
                     1 - dir, cross_index, cross_set);
}

static inline void board_set_cross_set_letter(Board *b, int row, int col,
                                              int dir, int cross_index,
                                              uint8_t letter) {
  grid_set_cross_set_letter(board_get_mutable_grid(b, b->transposed), row, col,
                            dir, cross_index, letter);
  grid_set_cross_set_letter(board_get_mutable_grid(b, 1 - b->transposed), col,
                            row, 1 - dir, cross_index, letter);
}

// Board: Cross score

static inline int board_get_cross_score(const Board *b, int row, int col,
                                        int dir, int cross_index) {
  return grid_get_cross_score(board_get_const_grid(b, b->transposed), row, col,
                              dir, cross_index);
}

static inline void board_set_cross_score(Board *b, int row, int col, int dir,
                                         int cross_index, int cross_score) {
  grid_set_cross_score(board_get_mutable_grid(b, b->transposed), row, col, dir,
                       cross_index, cross_score);
  grid_set_cross_score(board_get_mutable_grid(b, 1 - b->transposed), col, row,
                       1 - dir, cross_index, cross_score);
}

// Board: Anchors

static inline bool board_get_anchor(const Board *b, int row, int col, int dir) {
  return grid_get_anchor(board_get_const_grid(b, b->transposed), row, col, dir);
}

static inline void board_set_anchor(Board *b, int row, int col, int dir,
                                    bool anchor) {
  grid_set_anchor(board_get_mutable_grid(b, b->transposed), row, col, dir,
                  anchor);
  grid_set_anchor(board_get_mutable_grid(b, 1 - b->transposed), col, row,
                  1 - dir, anchor);
}

// This bypasses the modification of the number of row anchors and
// should only be used when resetting the board. Do not use this when
// updating the board after a play.
static inline void board_reset_anchor(Board *b, int row, int col, int dir) {
  grid_reset_anchor(board_get_mutable_grid(b, b->transposed), row, col, dir);
  grid_reset_anchor(board_get_mutable_grid(b, 1 - b->transposed), col, row,
                    1 - dir);
}

static inline void board_reset_number_of_anchor_rows(Board *b) {
  for (int i = 0; i < 2; i++) {
    Grid *g = board_get_mutable_grid(b, i);
    for (int i = 0; i < BOARD_DIM; i++) {
      g->number_of_row_anchors[i] = 0;
    }
  }
}

// Board: is cross word

static inline bool board_get_is_cross_word(const Board *b, int row, int col) {
  return grid_get_is_cross_word(board_get_const_grid(b, b->transposed), row,
                                col);
}

static inline void board_reset_is_cross_word(Board *b, int row, int col) {
  grid_reset_is_cross_word(board_get_mutable_grid(b, b->transposed), row, col);
  grid_reset_is_cross_word(board_get_mutable_grid(b, 1 - b->transposed), col,
                           row);
}

// Board: Transposed

static inline bool board_get_transposed(const Board *board) {
  return board->transposed;
}

static inline void board_transpose(Board *board) {
  board->transposed = !board->transposed;
}

static inline void board_set_transposed(Board *board, bool transposed) {
  board->transposed = transposed;
}

// Board: tiles played

static inline int board_get_tiles_played(const Board *board) {
  return board->tiles_played;
}

static inline void board_increment_tiles_played(Board *board,
                                                int tiles_played) {
  board->tiles_played += tiles_played;
}

// Board: traverse backwards return values

static inline bool board_get_path_is_valid(const Board *board) {
  return board->path_is_valid;
}

static inline void board_set_path_is_valid(Board *board, bool value) {
  board->path_is_valid = value;
}

static inline uint32_t board_get_node_index(const Board *board) {
  return board->node_index;
}

static inline void board_set_node_index(Board *board, uint32_t value) {
  board->node_index = value;
}

// Board auxilllary functions

static inline int board_get_cross_set_index(bool kwgs_are_shared,
                                            int player_index) {
  return (!kwgs_are_shared) && player_index;
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

static inline void board_clear_cross_set(Board *board, int row, int col,
                                         int dir, int cross_index) {
  board_set_cross_set(board, row, col, dir, cross_index, 0);
}

static inline void board_set_all_crosses(Board *board) {
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      for (int dir = 0; dir < 2; dir++) {
        for (int cross_index = 0; cross_index < 2; cross_index++) {
          board_set_cross_set(board, row, col, dir, cross_index,
                              TRIVIAL_CROSS_SET);
        }
      }
    }
  }
}

static inline void board_reset_all_cross_scores(Board *board) {
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      for (int dir = 0; dir < 2; dir++) {
        for (int cross_index = 0; cross_index < 2; cross_index++) {
          board_set_cross_score(board, row, col, dir, cross_index, 0);
        }
      }
    }
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

static inline bool board_matches_dir(const Board *board, int dir) {
  return (board_is_dir_vertical(dir) && board_get_transposed(board)) ||
         (!board_is_dir_vertical(dir) && !board_get_transposed(board));
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

  board_set_anchor(board, row, col, BOARD_HORIZONTAL_DIRECTION, false);
  board_set_anchor(board, row, col, BOARD_VERTICAL_DIRECTION, false);
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
      board_set_anchor(board, row, col, BOARD_HORIZONTAL_DIRECTION, true);
    }
    if (!tile_below) {
      board_set_anchor(board, row, col, BOARD_VERTICAL_DIRECTION, true);
    }
  } else {
    if (!tile_left && !tile_right && (tile_above || tile_below)) {
      board_set_anchor(board, row, col, BOARD_HORIZONTAL_DIRECTION, true);
    }
    if (!tile_above && !tile_below && (tile_left || tile_right)) {
      board_set_anchor(board, row, col, BOARD_VERTICAL_DIRECTION, true);
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
    board_reset_number_of_anchor_rows(board);
    board_set_anchor(board, BOARD_DIM / 2, BOARD_DIM / 2, 0, true);
  }
}

// FIXME: this can be faster, just do memset 0
// then set the trivial cross sets but also account
// for start square
static inline void board_reset(Board *board) {
  // The transposed field must be set to 0 here because
  // it is used to calculate the index for board_set_letter.
  board->tiles_played = 0;
  board->transposed = false;

  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      board_set_letter(board, row, col, ALPHABET_EMPTY_SQUARE_MARKER);
      board_reset_is_cross_word(board, row, col);
    }
  }

  board_set_all_crosses(board);
  board_reset_all_cross_scores(board);
  board_update_all_anchors(board);
}

static inline void board_set_bonus_squares(Board *b) {
  int i = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      uint8_t bonus_value;
      char bonus_square = CROSSWORD_GAME_BOARD[i++];
      if (bonus_square == BONUS_TRIPLE_WORD_SCORE) {
        bonus_value = 0x31;
      } else if (bonus_square == BONUS_DOUBLE_WORD_SCORE) {
        bonus_value = 0x21;
      } else if (bonus_square == BONUS_DOUBLE_LETTER_SCORE) {
        bonus_value = 0x12;
      } else if (bonus_square == BONUS_TRIPLE_LETTER_SCORE) {
        bonus_value = 0x13;
      } else {
        bonus_value = 0x11;
      }
      // Don't transpose the row and col when setting bonus squares
      grid_set_bonus_square(board_get_mutable_grid(b, 0), row, col,
                            bonus_value);
      grid_set_bonus_square(board_get_mutable_grid(b, 1), row, col,
                            bonus_value);
    }
  }
}

static inline Board *board_create() {
  Board *board = malloc_or_die(sizeof(Board));
  board_reset(board);
  board_set_bonus_squares(board);
  return board;
}

static inline void board_copy(Board *dst, const Board *src) {
  memory_copy(dst, src, sizeof(Board));
}

static inline Board *board_duplicate(const Board *board) {
  Board *new_board = malloc_or_die(sizeof(Board));
  board_copy(new_board, board);
  return new_board;
}

static inline void board_destroy(Board *board) {
  if (!board) {
    return;
  }
  free(board);
}

// FIXME: use movegen cache square
static inline void board_load_number_of_row_anchors_cache(const Board *b,
                                                          int *cache) {
  size_t rows_size = sizeof(int) * BOARD_DIM;
  memory_copy(cache,
              board_get_const_grid(b, b->transposed)->number_of_row_anchors,
              rows_size);
  memory_copy(cache + BOARD_DIM,
              board_get_const_grid(b, 1 - b->transposed)->number_of_row_anchors,
              rows_size);
}

// FIXME: remove if pointer assignment is faster
static inline void board_load_row_cache(const Board *b, int row,
                                        bool transposed, Square *squares) {
  memory_copy(squares,
              board_get_const_grid(b, transposed)->squares + row * BOARD_DIM,
              sizeof(Square) * BOARD_DIM);
}

#endif