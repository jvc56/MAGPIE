#ifndef MOVE_UNDO_H
#define MOVE_UNDO_H

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "equity.h"
#include <stdbool.h>
#include <stdint.h>

// Maximum number of square changes that can be tracked in a single move.
// A 15-tile play across the board would modify at most 15 squares.
enum { MAX_SQUARE_CHANGES = 16 };

// Structure to track a single square change for undo purposes.
typedef struct SquareChange {
  uint8_t row;
  uint8_t col;
  MachineLetter old_letter;
} SquareChange;

// Structure to efficiently track all changes made during a single move,
// allowing fast incremental play and unplay without full board copies.
typedef struct MoveUndo {
  // Square changes tracking
  SquareChange square_changes[MAX_SQUARE_CHANGES];
  uint8_t num_square_changes;

  // Bitmap to prevent duplicate square tracking (for 15x15 board = 225 bits)
  uint32_t squares_changed_bitmap[8];

  // Board scalars
  int old_tiles_played;
  bool old_cross_sets_valid;

  // Player state (both players - needed for game end scoring)
  uint16_t old_rack_array[MAX_ALPHABET_SIZE];
  uint8_t old_rack_count;
  Equity old_score;
  Equity old_opponent_score;
  int old_player_on_turn_index;

  // Bag state
  int old_bag_start_tile_index;
  int old_bag_end_tile_index;
  MachineLetter drawn_tiles[RACK_SIZE];
  uint8_t num_drawn_tiles;

  // Game state
  int old_consecutive_scoreless_turns;
  game_end_reason_t old_game_end_reason;

  // Move info for cross-set restoration (from second commit)
  uint8_t move_row_start;
  uint8_t move_col_start;
  uint8_t move_tiles_length;
  uint8_t move_dir;
  uint16_t move_played_tiles_mask; // Bitmask: 1 = played tile, 0 = play-through
} MoveUndo;

// Initialize a MoveUndo structure
static inline void move_undo_init(MoveUndo *undo) {
  undo->num_square_changes = 0;
  undo->num_drawn_tiles = 0;
  for (int i = 0; i < 8; i++) {
    undo->squares_changed_bitmap[i] = 0;
  }
}

// Check if a square has already been tracked
static inline bool move_undo_square_tracked(const MoveUndo *undo, int row,
                                            int col) {
  int idx = row * BOARD_DIM + col;
  int word_idx = idx / 32;
  int bit_idx = idx % 32;
  return (undo->squares_changed_bitmap[word_idx] & (1U << bit_idx)) != 0;
}

// Mark a square as tracked
static inline void move_undo_mark_square(MoveUndo *undo, int row, int col) {
  int idx = row * BOARD_DIM + col;
  int word_idx = idx / 32;
  int bit_idx = idx % 32;
  undo->squares_changed_bitmap[word_idx] |= (1U << bit_idx);
}

// Track a square change if not already tracked
static inline void move_undo_track_square(MoveUndo *undo, int row, int col,
                                          MachineLetter old_letter) {
  if (!move_undo_square_tracked(undo, row, col)) {
    move_undo_mark_square(undo, row, col);
    undo->square_changes[undo->num_square_changes].row = (uint8_t)row;
    undo->square_changes[undo->num_square_changes].col = (uint8_t)col;
    undo->square_changes[undo->num_square_changes].old_letter = old_letter;
    undo->num_square_changes++;
  }
}

// Track a drawn tile
static inline void move_undo_track_drawn_tile(MoveUndo *undo,
                                              MachineLetter tile) {
  undo->drawn_tiles[undo->num_drawn_tiles] = tile;
  undo->num_drawn_tiles++;
}

#endif
