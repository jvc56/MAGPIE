#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "letter_distribution.h"

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

Board *board_create();
Board *board_duplicate(const Board *board);
void board_copy(Board *dst, const Board *src);
void board_destroy(Board *board);

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

board_layout_t
board_layout_string_to_board_layout(const char *board_layout_string);
int board_score_move(const Board *board, const LetterDistribution *ld,
                     uint8_t word[], int word_start_index, int word_end_index,
                     int row, int col, int tiles_played, int cross_dir,
                     int cross_set_index);

static inline int board_get_cross_set_index(bool kwgs_are_shared,
                                            int player_index) {
  return (!kwgs_are_shared) && player_index;
}

void board_reset(Board *board);

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

void board_update_anchors(Board *board, int row, int col, int dir);
void board_update_all_anchors(Board *board);

#endif