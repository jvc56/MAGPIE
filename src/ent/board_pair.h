#ifndef BOARD_PAIR_H
#define BOARD_PAIR_H

#include "board.h"

#include "../util/util.h"

typedef struct BoardPair {
  Board *boards[2];
} BoardPair;

static inline BoardPair *board_pair_create() {
  BoardPair *bp = malloc_or_die(sizeof(BoardPair));
  bp->boards[0] = board_create();
  bp->boards[1] = board_create();
  return bp;
}

static inline BoardPair *board_pair_duplicate(BoardPair *bp) {
  BoardPair *new_bp = board_pair_create();
  board_copy(new_bp->boards[0], bp->boards[0]);
  board_copy(new_bp->boards[1], bp->boards[1]);
  return new_bp;
}

static inline void board_pair_copy(BoardPair *dst, const BoardPair *src) {
  board_copy(dst->boards[0], src->boards[0]);
  board_copy(dst->boards[1], src->boards[1]);
}

static inline void board_pair_destroy(BoardPair *bp) {
  board_destroy(bp->boards[0]);
  board_destroy(bp->boards[1]);
  free(bp);
}

static inline void board_pair_reset(BoardPair *bp) {
  board_reset(bp->boards[0]);
  board_reset(bp->boards[1]);
}

static inline Board *board_pair_get_board(BoardPair *bp, int board_index) {
  return bp->boards[board_index];
}

static inline void board_pair_set_letter(BoardPair *bp, int row, int col,
                                         uint8_t letter) {
  board_set_letter(bp->boards[0], row, col, letter);
  board_set_letter(bp->boards[1], col, row, letter);
}

static inline void board_pair_set_cross_score(BoardPair *bp, int row, int col,
                                              int score, int dir,
                                              int cross_set_index) {
  board_set_cross_score(bp->boards[0], row, col, score, dir, cross_set_index);
  board_set_cross_score(bp->boards[1], col, row, score, 1 - dir,
                        cross_set_index);
}

static inline void board_pair_set_cross_set(BoardPair *bp, int row, int col,
                                            uint64_t cross_set, int dir,
                                            int cross_set_index) {
  board_set_cross_set(bp->boards[0], row, col, cross_set, dir, cross_set_index);
  board_set_cross_set(bp->boards[1], col, row, cross_set, 1 - dir,
                      cross_set_index);
}

static inline void board_pair_set_anchor(BoardPair *bp, int row, int col,
                                         int dir) {
  board_set_anchor(bp->boards[0], row, col, dir);
  board_set_anchor(bp->boards[1], col, row, 1 - dir);
}

static inline void board_pair_reset_anchor(BoardPair *bp, int row, int col,
                                           int dir) {
  board_reset_anchor(bp->boards[0], row, col, dir);
  board_reset_anchor(bp->boards[1], col, row, 1 - dir);
}

static inline void board_pair_increment_tiles_played(BoardPair *bp,
                                                     int tiles_played) {
  board_increment_tiles_played(bp->boards[0], tiles_played);
  board_increment_tiles_played(bp->boards[1], tiles_played);
}

static inline void board_pair_transpose(BoardPair *bp) {
  board_transpose(bp->boards[0]);
  board_transpose(bp->boards[1]);
}

static inline void board_pair_update_anchors(BoardPair *bp, int row, int col,
                                             int dir) {
  if (board_is_dir_vertical(dir)) {
    int temp = row;
    row = col;
    col = temp;
  }

  board_pair_reset_anchor(bp, row, col, 0);
  board_pair_reset_anchor(bp, row, col, 1);
  bool tile_above = false;
  bool tile_below = false;
  bool tile_left = false;
  bool tile_right = false;
  bool tile_here = false;

  // Use board for access only, all writes should
  // be to the board pair.
  const Board *board = board_pair_get_board(bp, 0);

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
      board_pair_set_anchor(bp, row, col, 0);
    }
    if (!tile_below) {
      board_pair_set_anchor(bp, row, col, 1);
    }
  } else {
    if (!tile_left && !tile_right && (tile_above || tile_below)) {
      board_pair_set_anchor(bp, row, col, 0);
    }
    if (!tile_above && !tile_below && (tile_left || tile_right)) {
      board_pair_set_anchor(bp, row, col, 1);
    }
  }
}

static inline void board_pair_update_all_anchors(BoardPair *bp) {
  // Use board for access only, all writes should
  // be to the board pair.
  const Board *board = board_pair_get_board(bp, 0);
  if (board_get_tiles_played(board) > 0) {
    for (int i = 0; i < BOARD_DIM; i++) {
      for (int j = 0; j < BOARD_DIM; j++) {
        board_pair_update_anchors(bp, i, j, 0);
      }
    }
  } else {
    for (int i = 0; i < BOARD_DIM; i++) {
      for (int j = 0; j < BOARD_DIM; j++) {
        board_pair_reset_anchor(bp, i, j, 0);
        board_pair_reset_anchor(bp, i, j, 1);
      }
    }
    int rc = BOARD_DIM / 2;
    board_pair_set_anchor(bp, rc, rc, 0);
  }
}

#endif
