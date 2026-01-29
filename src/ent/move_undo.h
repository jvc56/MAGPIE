#ifndef MOVE_UNDO_H
#define MOVE_UNDO_H

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/rack_defs.h"
#include "board.h"
#include "rack.h"

// Maximum squares that can be modified by a single move.
// This calculation assumes RACK_SIZE=7. Other factors (SmallMove encoding)
// block endgame support for 8+ tile racks, so larger support is not needed.
// A 7-tile move can affect:
// - 7 tiles × 4 sub-boards for letters + is_cross_word = ~84
// - Cross sets: ~30 squares × specific dir/ci = ~60
// - Anchors: ~30 squares × 2 directions = ~60
// Total: ~200, round up to 256 for safety
#define MAX_UNDO_SQUARE_CHANGES 256

typedef struct SquareChange {
  int16_t index; // flat index into Board.squares
  Square old_value;
} SquareChange;

typedef struct MoveUndo {
  // Square changes - tracked incrementally
  int num_square_changes;
  SquareChange square_changes[MAX_UNDO_SQUARE_CHANGES];

  // Bitmap to track which square indices have been saved (avoid duplicates)
  // Board has 2*2*BOARD_DIM*BOARD_DIM squares (e.g. 900 for BOARD_DIM=15)
  // We use a bitmap: ceil(num_squares / 64) uint64_t words (e.g. 15 for 900).
  // The +63 in the expression ensures proper rounding up to the next 64-bit
  // word.
  uint64_t saved_squares_bitmap[(2 * 2 * BOARD_DIM * BOARD_DIM + 63) / 64];

  // Board scalar state
  int old_tiles_played;
  uint8_t old_number_of_row_anchors[BOARD_DIM * 2];
  bool old_cross_sets_valid;

  // Player on turn state
  int player_on_turn_index;
  Rack old_rack;
  Equity old_scores[2]; // Both players' scores (needed for end game penalty)

  // Tiles drawn from bag (to put back on undo)
  MachineLetter tiles_drawn[RACK_SIZE];
  int num_tiles_drawn;

  // Bag state - we save start/end indices and PRNG state
  // The letters array doesn't change, just the indices
  int old_bag_start_tile_index;
  int old_bag_end_tile_index;

  // Game state
  int old_consecutive_scoreless_turns;
  game_end_reason_t old_game_end_reason;

  // Minimal move info for lazy cross-set updates
  // Only valid if tiles were placed (not pass/exchange)
  uint8_t move_row_start;
  uint8_t move_col_start;
  uint8_t move_tiles_length;
  uint8_t move_dir;
  // Bitmask: bit i is set if position i had an actual tile placed (not
  // played-through) Max tiles_length is BOARD_DIM (15), so 16 bits is
  // sufficient
  uint16_t tiles_placed_mask;
} MoveUndo;

static inline void move_undo_reset(MoveUndo *undo) {
  undo->num_square_changes = 0;
  undo->num_tiles_drawn = 0;
  undo->move_tiles_length = 0;
  memset(undo->saved_squares_bitmap, 0, sizeof(undo->saved_squares_bitmap));
}

static inline bool move_undo_square_is_saved(const MoveUndo *undo, int index) {
  return (undo->saved_squares_bitmap[index / 64] >> (index % 64)) & 1;
}

static inline void move_undo_mark_square_saved(MoveUndo *undo, int index) {
  undo->saved_squares_bitmap[index / 64] |= ((uint64_t)1 << (index % 64));
}

// Save a square if not already saved
static inline void move_undo_save_square(MoveUndo *undo, Board *board,
                                         int index) {
  if (move_undo_square_is_saved(undo, index)) {
    return;
  }
  if (undo->num_square_changes >= MAX_UNDO_SQUARE_CHANGES) {
    log_fatal("move_undo: exceeded MAX_UNDO_SQUARE_CHANGES");
  }
  undo->square_changes[undo->num_square_changes].index = (int16_t)index;
  undo->square_changes[undo->num_square_changes].old_value =
      board->squares[index];
  undo->num_square_changes++;
  move_undo_mark_square_saved(undo, index);
}

// Restore all saved squares
static inline void move_undo_restore_squares(const MoveUndo *undo,
                                             Board *board) {
  for (int i = undo->num_square_changes - 1; i >= 0; i--) {
    board->squares[undo->square_changes[i].index] =
        undo->square_changes[i].old_value;
  }
}

// Helper to save square by row/col/dir/ci before modification
static inline void move_undo_save_square_at(MoveUndo *undo, Board *board,
                                            int row, int col, int dir, int ci) {
  int index = board_get_square_index(board, row, col, dir, ci);
  move_undo_save_square(undo, board, index);
}

// Tracked version of board_set_letter
// Saves all squares that will be modified before modifying them
static inline void board_set_letter_tracked(Board *b, int row, int col,
                                            MachineLetter letter,
                                            MoveUndo *undo) {
  // Save and set letter on all 4 squares
  for (int ci = 0; ci < 2; ci++) {
    for (int dir = 0; dir < 2; dir++) {
      move_undo_save_square_at(undo, b, row, col, dir, ci);
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
          move_undo_save_square_at(undo, b, left_cw_row, left_cw_col, dir, ci);
          square_set_is_cross_word(
              board_get_writable_square(b, left_cw_row, left_cw_col, dir, ci),
              true);
        }
        if (inc_right) {
          move_undo_save_square_at(undo, b, right_cw_row, right_cw_col, dir,
                                   ci);
          square_set_is_cross_word(
              board_get_writable_square(b, right_cw_row, right_cw_col, dir, ci),
              true);
        }
      }
    }
  }
}

// Tracked version of board_set_cross_set
static inline void board_set_cross_set_tracked(Board *b, int row, int col,
                                               int dir, int ci,
                                               uint64_t cross_set,
                                               MoveUndo *undo) {
  move_undo_save_square_at(undo, b, row, col, dir, ci);
  square_set_cross_set(board_get_writable_square(b, row, col, dir, ci),
                       cross_set);
}

// Tracked version of board_set_cross_set_with_blank
static inline void board_set_cross_set_with_blank_tracked(Board *b, int row,
                                                          int col, int dir,
                                                          int ci,
                                                          uint64_t cross_set,
                                                          MoveUndo *undo) {
  move_undo_save_square_at(undo, b, row, col, dir, ci);
  const uint64_t cross_set_with_blank = cross_set + !!cross_set;
  square_set_cross_set(board_get_writable_square(b, row, col, dir, ci),
                       cross_set_with_blank);
}

// Tracked version of board_set_cross_score
static inline void board_set_cross_score_tracked(Board *b, int row, int col,
                                                 int dir, int ci,
                                                 Equity cross_score,
                                                 MoveUndo *undo) {
  move_undo_save_square_at(undo, b, row, col, dir, ci);
  square_set_cross_score(board_get_writable_square(b, row, col, dir, ci),
                         cross_score);
}

// Tracked version of board_set_anchor
// Must set anchor on BOTH cross indices (ci=0 and ci=1), same as
// board_set_anchor Also updates number_of_row_anchors like the original
static inline bool board_set_anchor_tracked(Board *board, int row, int col,
                                            int dir, bool anchor,
                                            MoveUndo *undo) {
  bool old_anchor = false;
  for (int ci = 0; ci < 2; ci++) {
    move_undo_save_square_at(undo, board, row, col, dir, ci);
    Square *s = board_get_writable_square(board, row, col, dir, ci);
    bool prev_anchor = s->anchor;
    s->anchor = anchor;
    if (ci == 0) {
      old_anchor = prev_anchor;
      // Update number_of_row_anchors - this is needed for correct move
      // generation
      const int index =
          board_get_number_of_row_anchors_index(board, row, col, dir);
      if (prev_anchor && !anchor) {
        board->number_of_row_anchors[index]--;
      } else if (!prev_anchor && anchor) {
        board->number_of_row_anchors[index]++;
      }
    }
  }
  return old_anchor;
}

// Tracked version of board_set_left_extension_set_with_blank
static inline void board_set_left_extension_set_with_blank_tracked(
    Board *b, int row, int col, int dir, int csi, uint64_t left_extension_set,
    MoveUndo *undo) {
  move_undo_save_square_at(undo, b, row, col, dir, csi);
  const uint64_t left_extension_set_with_blank =
      left_extension_set + !!left_extension_set;
  square_set_left_extension_set(
      board_get_writable_square(b, row, col, dir, csi),
      left_extension_set_with_blank);
}

// Tracked version of board_set_right_extension_set_with_blank
static inline void board_set_right_extension_set_with_blank_tracked(
    Board *b, int row, int col, int dir, int csi, uint64_t right_extension_set,
    MoveUndo *undo) {
  move_undo_save_square_at(undo, b, row, col, dir, csi);
  const uint64_t right_extension_set_with_blank =
      right_extension_set + !!right_extension_set;
  square_set_right_extension_set(
      board_get_writable_square(b, row, col, dir, csi),
      right_extension_set_with_blank);
}

// Tracked version of board_update_anchors - must match original exactly
static inline void board_update_anchors_tracked(Board *board, int row, int col,
                                                MoveUndo *undo) {
  // Save both direction squares, both cross indices, before modifying
  for (int ci = 0; ci < 2; ci++) {
    move_undo_save_square_at(undo, board, row, col, BOARD_HORIZONTAL_DIRECTION,
                             ci);
    move_undo_save_square_at(undo, board, row, col, BOARD_VERTICAL_DIRECTION,
                             ci);
  }

  board_set_anchor_tracked(board, row, col, BOARD_HORIZONTAL_DIRECTION, false,
                           undo);
  board_set_anchor_tracked(board, row, col, BOARD_VERTICAL_DIRECTION, false,
                           undo);
  if (board_get_is_brick(board, row, col)) {
    return;
  }
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
    // When there's a tile here, set anchors based on adjacent empty squares
    if (!tile_right) {
      board_set_anchor_tracked(board, row, col, BOARD_HORIZONTAL_DIRECTION,
                               true, undo);
    }
    if (!tile_below) {
      board_set_anchor_tracked(board, row, col, BOARD_VERTICAL_DIRECTION, true,
                               undo);
    }
  } else {
    // Empty square: set anchors only if adjacent tiles exist in specific
    // pattern
    if (!tile_left && !tile_right && (tile_above || tile_below)) {
      board_set_anchor_tracked(board, row, col, BOARD_HORIZONTAL_DIRECTION,
                               true, undo);
    }
    if (!tile_above && !tile_below && (tile_left || tile_right)) {
      board_set_anchor_tracked(board, row, col, BOARD_VERTICAL_DIRECTION, true,
                               undo);
    }
  }
}

#endif
