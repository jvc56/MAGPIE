#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"

#include "letter_distribution.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

typedef struct Square {
  uint8_t letter;
  uint8_t bonus_square;
  uint64_t cross_set;
  int cross_score;
  bool anchor;
  // FIXME: this should be a bool
  int is_cross_word;
} Square;

// Board maintains four squares:
// - One pair for each direction
// - One pair for each cross index
typedef struct Board {
  // We use (number of squares * 4) for cross sets
  // to account for
  //   - vertical and horizontal board directions
  //   - separate lexicons used by player 1 and player 2

  // We use (number of squares * 4) for cross scores
  // for reasons listed above.

  // We use (number of squares * 2) for cross sets
  // to account for
  //   - vertical and horizontal board directions
  Square squares[2 * 2 * BOARD_DIM * BOARD_DIM];
  int number_of_row_anchors[BOARD_DIM * 2];
  int transposed;
  int tiles_played;
  // Scratch pad for return values used by
  // traverse backwards for score
  uint32_t node_index;
  bool path_is_valid;
} Board;

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

static inline uint64_t square_get_cross_set(const Square *s) {
  return s->cross_set;
}

static inline void square_set_cross_set(Square *s, uint64_t cross_set) {
  s->cross_set = cross_set;
}

static inline uint64_t get_cross_set_bit(uint8_t letter) {
  return (uint64_t)1 << letter;
}

static inline void square_set_cross_set_letter(Square *s, uint8_t letter) {
  s->cross_set |= get_cross_set_bit(letter);
}

// Square: Cross scores

static inline int square_get_cross_score(const Square *s) {
  return s->cross_score;
}

static inline void square_set_cross_score(Square *s, int score) {
  s->cross_score = score;
}

// Square: Anchors

static inline bool square_get_anchor(const Square *s) { return s->anchor; }

// Returns the previous value of the anchor before it was set.
static inline bool square_set_anchor(Square *s, bool anchor) {
  bool old = s->anchor;
  s->anchor = anchor;
  return old;
}

static inline void square_reset_anchor(Square *s) { s->anchor = false; }

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

// Square getter helpers

// Square getter helpers

static inline int board_get_square_index(const Board *b, int row, int col,
                                         int dir, int ci) {
  const int cross_offset = ci * 2 * BOARD_DIM * BOARD_DIM;
  const int adjusted_dir = (dir ^ b->transposed);
  const int dir_offset = adjusted_dir * BOARD_DIM * BOARD_DIM;

  int row_offset = 0;
  int col_offset = 0;
  int index = 0;
  if (!dir) {
    row_offset = row * BOARD_DIM;
    col_offset = col;
  } else {
    row_offset = col * BOARD_DIM;
    col_offset = row;
  }
  index = cross_offset + dir_offset + row_offset + col_offset;
  // printf("bgsi: %d, %d, %d, %d: %d + %d + %d + %d = %d\n", row, col, dir, ci,
  //        cross_offset, dir_offset, row_offset, col_offset, index);
  return index;
}

static inline Square *board_get_writable_square(Board *b, int row, int col,
                                                int dir, int ci) {
  return &b->squares[board_get_square_index(b, row, col, dir, ci)];
}

static inline const Square *
board_get_readonly_square(const Board *b, int row, int col, int dir, int ci) {
  const int index = board_get_square_index(b, row, col, dir, ci);
  // printf("sq idx: %p\n", &b->squares[index]);
  return &b->squares[index];
}

static inline void print_square(const Square *s) {
  uint8_t l = square_get_letter(s);
  uint8_t bs = square_get_bonus_square(s);
  int score = square_get_cross_score(s);
  bool a = square_get_anchor(s);
  int ic = square_get_is_cross_word(s);
  uint64_t set = square_get_cross_set(s);

  const char *bs_string;
  if (bs == 0x31) {
    bs_string = "Triple Word";
  } else if (bs == 0x21) {
    bs_string = "Double Word";
  } else if (bs == 0x12) {
    bs_string = "Double Letter";
  } else if (bs == 0x13) {
    bs_string = "Triple Letter";
  } else {
    bs_string = "None";
  }

  printf(">%c,%s,%d,%d,%d,", 'A' + l - 1, bs_string, score, a, ic);
  for (int i = 0; i < 27; i++) {
    if ((set >> i) & 1) {
      if (i == 0) {
        printf("?");
      } else {
        printf("%c", 'A' + i - 1);
      }
    }
  }
  printf("<\n");
}

static inline void print_squares(const Board *b, int row, int col) {
  printf("\n\n\nprinting the square\n\n\n");
  for (int dir = 0; dir < 2; dir++) {
    for (int ci = 0; ci < 2; ci++) {
      printf("indexes for square: %d, %d, %d, %d\n", row, col, dir, ci);
      print_square(board_get_readonly_square(b, row, col, dir, ci));
    }
  }
}

static inline void print_board(const Board *b) {
  printf("\n\n\nPB\n\n\n");
  for (int ci = 0; ci < 2; ci++) {
    for (int dir = 0; dir < 2; dir++) {
      for (int row = 0; row < BOARD_DIM; row++) {
        printf("\n\nsquares for %d, %d, %d:\n", ci, dir, row);
        for (int col = 0; col < BOARD_DIM; col++) {
          printf("\n\ncol %d:\n", col);
          print_square(board_get_readonly_square(b, row, col, dir, ci));
        }
      }
    }
  }
}

// Board: Letter

static inline uint8_t board_get_letter(const Board *b, int row, int col) {
  // Cross index doesn't matter for letter reads.
  return square_get_letter(board_get_readonly_square(b, row, col, 0, 0));
}

static inline void board_set_letter(Board *b, int row, int col,
                                    uint8_t letter) {
  // Letter should be set on all 4 squares.
  // printf("setting letter: %d, %d, %c\n", row, col, letter + 'A' - 1);
  for (int ci = 0; ci < 2; ci++) {
    for (int dir = 0; dir < 2; dir++) {
      square_set_letter(board_get_writable_square(b, row, col, dir, ci),
                        letter);
      if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
        int left_cw_row = row;
        int left_cw_col = col;
        int right_cw_row = row;
        int right_cw_col = col;
        bool inc_left = true;
        bool inc_right = true;
        if (dir == BOARD_HORIZONTAL_DIRECTION) {
          left_cw_row--;
          right_cw_row++;
          inc_left = left_cw_row >= 0;
          inc_right = right_cw_row < BOARD_DIM;
        } else {
          left_cw_col--;
          right_cw_col++;
          inc_left = left_cw_col >= 0;
          inc_right = right_cw_col < BOARD_DIM;
        }
        if (inc_left) {
          // printf("inc cw: %d, %d, %d, %d\n", left_cw_row, left_cw_col, dir,
          // ci);
          square_increment_is_cross_word(
              board_get_writable_square(b, left_cw_row, left_cw_col, dir, ci));
        }
        if (inc_right) {
          // printf("inc cw: %d, %d, %d, %d\n", right_cw_row, right_cw_col, dir,
          //        ci);
          square_increment_is_cross_word(board_get_writable_square(
              b, right_cw_row, right_cw_col, dir, ci));
        }
        // printf("done cw\n\n");
      }
    }
  }
  // printf("done setting letter\n");
}

// Board: Bonus square

static inline uint8_t board_get_bonus_square(const Board *b, int row, int col) {
  // Cross index doesn't matter for bonus square reads.
  return square_get_bonus_square(board_get_readonly_square(b, row, col, 0, 0));
}

static inline void board_set_bonus_square(Board *b, int row, int col,
                                          uint8_t bonus_square) {
  // Bonus square should be set on all 4 squares.
  for (int ci = 0; ci < 2; ci++) {
    for (int dir = 0; dir < 2; dir++) {
      square_set_bonus_square(board_get_writable_square(b, row, col, dir, ci),
                              bonus_square);
    }
  }
}

// Board: Cross set

static inline uint64_t board_get_cross_set(const Board *b, int row, int col,
                                           int dir, int ci) {
  return square_get_cross_set(board_get_readonly_square(b, row, col, dir, ci));
}

static inline void board_set_cross_set(Board *b, int row, int col, int dir,
                                       int ci, uint64_t cross_set) {
  square_set_cross_set(board_get_writable_square(b, row, col, dir, ci),
                       cross_set);
  // printf("set cross %d, %d, %d, %d: %ld\n", row, col, dir, ci,
  //        square_get_cross_set(board_get_readonly_square(b, row, col, dir,
  //        ci)));
}

static inline void board_set_cross_set_letter(Board *b, int row, int col,
                                              int dir, int ci, uint8_t letter) {
  square_set_cross_set_letter(board_get_writable_square(b, row, col, dir, ci),
                              letter);
  // printf("set letter cross %d, %d, %d, %d, %d: %ld\n", row, col, dir, ci,
  //        'A' + letter - 1,
  //        square_get_cross_set(board_get_readonly_square(b, row, col, dir,
  //        ci)));
}

// Board: Cross score

static inline int board_get_cross_score(const Board *b, int row, int col,
                                        int dir, int ci) {
  return square_get_cross_score(
      board_get_readonly_square(b, row, col, dir, ci));
}

static inline void board_set_cross_score(Board *b, int row, int col, int dir,
                                         int ci, int cross_score) {
  square_set_cross_score(board_get_writable_square(b, row, col, dir, ci),
                         cross_score);
}

// Board: Anchors

static inline int board_get_number_of_row_anchors_index(const Board *b, int row,
                                                        int col, int dir) {
  int index = BOARD_DIM * (dir ^ b->transposed);
  if (dir == BOARD_HORIZONTAL_DIRECTION) {
    index += row;
  } else {
    index += col;
  }
  return index;
}

static inline void update_number_of_row_anchors(Board *b, int row, int col,
                                                int dir, bool old_anchor,
                                                bool new_anchor) {
  const int index = board_get_number_of_row_anchors_index(b, row, col, dir);
  if (old_anchor && !new_anchor) {
    b->number_of_row_anchors[index]--;
  } else if (!old_anchor && new_anchor) {
    b->number_of_row_anchors[index]++;
  }
}

static inline int board_get_number_of_row_anchors(const Board *board,
                                                  int row_or_col, int dir) {
  if (board->transposed) {
    log_fatal("cannot get number of row anchors for the transposed board\n");
  }
  return board->number_of_row_anchors[board_get_number_of_row_anchors_index(
      board, row_or_col, row_or_col, dir)];
}

static inline bool board_get_anchor(const Board *b, int row, int col, int dir) {
  return square_get_anchor(board_get_readonly_square(b, row, col, dir, 0));
}

static inline void board_set_anchor(Board *b, int row, int col, int dir,
                                    bool anchor) {
  // Anchors should be set on all 4 squares.
  for (int ci = 0; ci < 2; ci++) {
    bool old_anchor = square_set_anchor(
        board_get_writable_square(b, row, col, dir, ci), anchor);
    if (ci == 0) {
      update_number_of_row_anchors(b, row, col, dir, old_anchor, anchor);
    }
  }
}

// This bypasses the modification of the number of row anchors and
// should only be used when resetting the board. Do not use this when
// updating the board after a play.
static inline void board_reset_anchor(Board *b, int row, int col, int dir) {
  for (int ci = 0; ci < 2; ci++) {
    square_reset_anchor(board_get_writable_square(b, row, col, dir, ci));
  }
}

static inline void board_reset_number_of_anchor_rows(Board *b) {
  for (int i = 0; i < BOARD_DIM * 2; i++) {
    b->number_of_row_anchors[i] = 0;
  }
}

// Board: is cross word

static inline bool board_get_is_cross_word(const Board *b, int row, int col,
                                           int dir) {
  // Cross index doesn't matter for is cross word
  // printf("getting is cross word\n");
  return square_get_is_cross_word(
      board_get_readonly_square(b, row, col, dir, 0));
}

static inline void board_reset_is_cross_word(Board *b, int row, int col,
                                             int dir) {
  for (int ci = 0; ci < 2; ci++) {
    square_reset_is_cross_word(board_get_writable_square(b, row, col, dir, ci));
  }
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

static inline bool board_matches_dir(const Board *board, int dir) {
  return (board_is_dir_vertical(dir) && board_get_transposed(board)) ||
         (!board_is_dir_vertical(dir) && !board_get_transposed(board));
}

static inline void board_clear_cross_set(Board *board, int row, int col,
                                         int dir, int ci) {
  board_set_cross_set(board, row, col, dir, ci, 0);
}

static inline void board_set_all_crosses(Board *board) {
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      for (int dir = 0; dir < 2; dir++) {
        for (int ci = 0; ci < 2; ci++) {
          board_set_cross_set(board, row, col, dir, ci, TRIVIAL_CROSS_SET);
        }
      }
    }
  }
}

static inline void board_reset_all_cross_scores(Board *board) {
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      for (int dir = 0; dir < 2; dir++) {
        for (int ci = 0; ci < 2; ci++) {
          board_set_cross_score(board, row, col, dir, ci, 0);
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
                                      int word_dir) {
  while (board_is_position_valid(row, col) &&
         !board_is_empty(board, row, col)) {
    col += word_dir;
  }
  return col - word_dir;
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

static inline void board_update_anchors(Board *board, int row, int col) {
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
        board_update_anchors(board, i, j);
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
  board->transposed = 0;

  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      board_set_letter(board, row, col, ALPHABET_EMPTY_SQUARE_MARKER);
      board_reset_is_cross_word(board, row, col, BOARD_HORIZONTAL_DIRECTION);
      board_reset_is_cross_word(board, row, col, BOARD_VERTICAL_DIRECTION);
    }
  }

  board_set_all_crosses(board);
  board_reset_all_cross_scores(board);
  board_update_all_anchors(board);
}

static inline void board_init_bonus_squares(Board *b) {
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
      board_set_bonus_square(b, row, col, bonus_value);
    }
  }
}

static inline Board *board_create() {
  Board *board = malloc_or_die(sizeof(Board));
  board_reset(board);
  board_init_bonus_squares(board);
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
  if (b->transposed) {
    log_fatal("cannot load row anchor cache while board is transposed\n");
  }
  memory_copy(cache, b->number_of_row_anchors, sizeof(int) * BOARD_DIM * 2);
}

// FIXME: remove if pointer assignment is faster
static inline void board_load_row_cache(const Board *b, int row_or_col, int dir,
                                        int ci, Square *squares) {
  if (b->transposed) {
    log_fatal("cannot load row cache while board is transposed\n");
  }
  // FIXME: if this is commented out, board tests still pass, plz fix
  int row = row_or_col;
  int col = row_or_col;
  if (dir == BOARD_HORIZONTAL_DIRECTION) {
    col = 0;
  } else {
    row = 0;
  }
  // printf("\n\nlsf %d,%d,%d\n", row, col, dir);
  memory_copy(squares, board_get_readonly_square(b, row, col, dir, ci),
              sizeof(Square) * BOARD_DIM);
  // for (int i = 0; i < BOARD_DIM; i++) {
  //   printf("\n\nsquare at %d\n", i);
  //   print_squares(b, row_or_col, i);
  //   printf("\ncol square\n");
  //   print_squares(b, i, row_or_col);
  //   printf("\nusing square: %p\n", &squares[i]);
  //   print_square(&squares[i]);
  // }
}

#endif