#include "incr_move_gen.h"

#include "../ent/move.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

IncrMoveList *incr_move_list_create(int capacity) {
  IncrMoveList *iml = (IncrMoveList *)malloc_or_die(sizeof(IncrMoveList));
  iml->moves = (IncrMove *)malloc_or_die(sizeof(IncrMove) * capacity);
  iml->count = 0;
  iml->capacity = capacity;
  memset(&iml->tile_mapping, 0, sizeof(IncrTileMapping));
  iml->player_index = -1;
  iml->rack_total_letters = 0;
  return iml;
}

void incr_move_list_destroy(IncrMoveList *iml) {
  if (!iml) {
    return;
  }
  free(iml->moves);
  free(iml);
}

void incr_move_list_reset(IncrMoveList *iml) { iml->count = 0; }

void incr_move_list_setup_tile_mapping(IncrMoveList *iml, const Rack *rack) {
  IncrTileMapping *mapping = &iml->tile_mapping;
  memset(mapping->tile_to_index, 0xFF, sizeof(mapping->tile_to_index));
  memset(mapping->index_to_tile, 0, sizeof(mapping->index_to_tile));
  mapping->num_unique_tiles = 0;
  mapping->rack_packed = 0;

  const int dist_size = rack_get_dist_size(rack);
  for (int ml = 0; ml < dist_size; ml++) {
    int count = rack_get_letter(rack, ml);
    if (count > 0) {
      int idx = mapping->num_unique_tiles;
      mapping->tile_to_index[ml] = (uint8_t)idx;
      mapping->index_to_tile[idx] = (uint8_t)ml;
      mapping->rack_packed |= ((uint32_t)count << (idx * 3));
      mapping->num_unique_tiles++;
    }
  }
}

uint32_t incr_move_compute_tiles_used(const IncrTileMapping *mapping,
                                      const SmallMove *sm) {
  uint32_t packed = 0;
  const int num_tiles = small_move_get_tiles_played(sm);
  const uint64_t tm = sm->tiny_move;

  for (int tile_idx = 0; tile_idx < num_tiles; tile_idx++) {
    MachineLetter tile = (MachineLetter)((tm >> (20 + 6 * tile_idx)) & 63);
    // If this tile is blanked, map to BLANK_MACHINE_LETTER (0)
    MachineLetter ml =
        (tm & (1ULL << (12 + tile_idx))) ? BLANK_MACHINE_LETTER : tile;
    int local_idx = mapping->tile_to_index[ml];
    // Increment the 3-bit count at position local_idx
    packed += (1U << (local_idx * 3));
  }
  return packed;
}

void incr_move_list_populate_from_small_moves(IncrMoveList *iml,
                                              const MoveList *ml,
                                              const Rack *rack,
                                              int player_index) {
  incr_move_list_reset(iml);
  incr_move_list_setup_tile_mapping(iml, rack);
  iml->player_index = player_index;
  iml->rack_total_letters = rack_get_total_letters(rack);

  // Ensure capacity
  if (ml->count > iml->capacity) {
    iml->capacity = ml->count;
    iml->moves =
        (IncrMove *)realloc_or_die(iml->moves, sizeof(IncrMove) * iml->capacity);
  }

  for (int move_idx = 0; move_idx < ml->count; move_idx++) {
    const SmallMove *sm = ml->small_moves[move_idx];
    IncrMove *im = &iml->moves[iml->count];
    im->small_move = *sm;
    if (small_move_is_pass(sm)) {
      im->tiles_and_anchor = 0;
    } else {
      uint32_t tiles_used =
          incr_move_compute_tiles_used(&iml->tile_mapping, sm);
      // No anchor tracking yet — set to 0
      incr_move_set_tiles_and_anchor(im, tiles_used, 0, 0, 0);
    }
    iml->count++;
  }
}

// Check if a filled square invalidates a SmallMove. The move is invalidated
// if the square falls within the move's span (overlap), OR if the square is
// immediately adjacent to the span endpoints in the move's direction (which
// would extend the main word, changing what the GADDAG would generate).
// Decode a SmallMove's position into original board coordinates.
// row/col/row_inc/col_inc match the traversal in small_move_to_move.
static void small_move_decode_position(const SmallMove *sm, int *row, int *col,
                                       int *row_inc, int *col_inc) {
  const uint64_t tm = sm->tiny_move;
  // small_move_to_move uses row=bits6-10, col=bits1-5 directly.
  // For vertical moves, the encoding already swapped row/col so that
  // row=original_col, col=original_row — but board_get_letter uses
  // (row, col) with HORIZONTAL direction, so these map to original
  // coordinates correctly: position (row, col) on the original board.
  *row = (int)((tm & SMALL_MOVE_ROW_BITMASK) >> 6);
  *col = (int)((tm & SMALL_MOVE_COL_BITMASK) >> 1);
  const bool vert = (tm & 1) != 0;
  *row_inc = vert ? 1 : 0;
  *col_inc = vert ? 0 : 1;
}

// sq_row/sq_col are in actual board coordinates (from MoveUndo).
// The SmallMove's decoded (row, col) from small_move_decode_position are
// also actual board coordinates (small_move_to_move uses them with
// board_get_letter which takes actual coords).
static bool incr_move_affected_by_square(const SmallMove *sm, int sq_row,
                                         int sq_col) {
  if (small_move_is_pass(sm)) {
    return false;
  }
  int row, col, row_inc, col_inc;
  small_move_decode_position(sm, &row, &col, &row_inc, &col_inc);
  const int play_length = small_move_get_play_length(sm);

  for (int pos = -1; pos <= play_length; pos++) {
    int r = row + pos * row_inc;
    int c = col + pos * col_inc;
    if (r == sq_row && c == sq_col) {
      return true;
    }
  }
  return false;
}

// Get the filled squares from a MoveUndo's recorded move info.
// Returns number of filled squares written to out_rows/out_cols.
static int incr_get_filled_squares(const MoveUndo *undo, int *out_rows,
                                   int *out_cols) {
  if (undo->move_tiles_length == 0) {
    return 0; // pass
  }
  int count = 0;
  const int row_start = undo->move_row_start;
  const int col_start = undo->move_col_start;
  const int dir = undo->move_dir;
  const uint16_t mask = undo->tiles_placed_mask;

  for (int pos_idx = 0; pos_idx < undo->move_tiles_length; pos_idx++) {
    if (!(mask & (1 << pos_idx))) {
      continue; // played-through, not a placed tile
    }
    if (dir == BOARD_HORIZONTAL_DIRECTION) {
      out_rows[count] = row_start;
      out_cols[count] = col_start + pos_idx;
    } else {
      out_rows[count] = row_start + pos_idx;
      out_cols[count] = col_start;
    }
    count++;
  }
  return count;
}

// Check if a SmallMove is still valid on the current board by verifying
// cross-sets at each position where the move places a tile. Also checks
// that the score is unchanged (cross-word scores may have changed).
// Returns false if the move is no longer valid or has a stale score.
// Check if any position in a SmallMove's span shares a row or column with
// any of the filled squares. This conservatively catches cross-set changes
// that propagate through connected tiles along a row/column.
static bool incr_move_shares_line_with_filled(const SmallMove *sm,
                                              const Board *board,
                                              const int *filled_rows,
                                              const int *filled_cols,
                                              int num_filled) {
  if (small_move_is_pass(sm) || num_filled == 0) {
    return false;
  }
  int row, col, row_inc, col_inc;
  small_move_decode_position(sm, &row, &col, &row_inc, &col_inc);
  const int play_length = small_move_get_play_length(sm);

  for (int pos = 0; pos < play_length; pos++) {
    int r = row + pos * row_inc;
    int c = col + pos * col_inc;

    if (!board_is_empty(board, r, c)) {
      continue; // played-through position, skip
    }

    // Check if any filled square shares a row or column with this position.
    // Same row → horizontal cross-set may have changed.
    // Same col → vertical cross-set may have changed.
    for (int sq_idx = 0; sq_idx < num_filled; sq_idx++) {
      if (filled_rows[sq_idx] == r || filled_cols[sq_idx] == c) {
        return true;
      }
    }
  }
  return false;
}

int incr_move_list_invalidate(IncrMoveList *iml, const MoveUndo *undo1,
                              const MoveUndo *undo2,
                              const Rack *remaining_rack,
                              const Board *board) {
  // Build packed rack counts for feasibility check
  IncrTileMapping temp_mapping;
  memset(temp_mapping.tile_to_index, 0xFF, sizeof(temp_mapping.tile_to_index));
  temp_mapping.num_unique_tiles = 0;
  temp_mapping.rack_packed = 0;

  // Build mapping from the original mapping's tile types, but with
  // remaining_rack counts
  const IncrTileMapping *orig = &iml->tile_mapping;
  for (int idx = 0; idx < orig->num_unique_tiles; idx++) {
    uint8_t ml = orig->index_to_tile[idx];
    int count = rack_get_letter(remaining_rack, ml);
    temp_mapping.rack_packed |= ((uint32_t)count << (idx * 3));
  }

  // Collect filled squares from both undos
  int filled_rows[RACK_SIZE * 2];
  int filled_cols[RACK_SIZE * 2];
  int num_filled = 0;
  if (undo1) {
    num_filled +=
        incr_get_filled_squares(undo1, filled_rows, filled_cols);
  }
  if (undo2) {
    num_filled += incr_get_filled_squares(undo2, filled_rows + num_filled,
                                          filled_cols + num_filled);
  }

  // Compact the list: keep only valid moves
  int write_idx = 0;
  for (int read_idx = 0; read_idx < iml->count; read_idx++) {
    IncrMove *im = &iml->moves[read_idx];
    bool invalid = false;

    // Check position overlap with filled squares
    for (int sq_idx = 0; sq_idx < num_filled; sq_idx++) {
      if (incr_move_affected_by_square(&im->small_move, filled_rows[sq_idx],
                                    filled_cols[sq_idx])) {
        invalid = true;
        break;
      }
    }

    // Check rack feasibility
    if (!invalid && !small_move_is_pass(&im->small_move)) {
      uint32_t tiles_used = incr_move_get_tiles_used(im);
      if (!incr_move_tiles_feasible(tiles_used, temp_mapping.rack_packed)) {
        invalid = true;
      }
    }

    // Check if any placed-tile position is adjacent to a newly-filled square
    // (cross-word validity or score may have changed)
    if (!invalid) {
      if (incr_move_shares_line_with_filled(&im->small_move, board, filled_rows,
                                       filled_cols, num_filled)) {
        invalid = true;
      }
    }

    if (!invalid) {
      if (write_idx != read_idx) {
        iml->moves[write_idx] = iml->moves[read_idx];
      }
      write_idx++;
    }
  }

  int removed = iml->count - write_idx;
  iml->count = write_idx;

  // Update the tile mapping to reflect the remaining rack
  iml->tile_mapping.rack_packed = temp_mapping.rack_packed;

  return removed;
}

void incr_move_list_copy_into(IncrMoveList *dest, const IncrMoveList *src) {
  if (src->count > dest->capacity) {
    dest->capacity = src->count;
    dest->moves = (IncrMove *)realloc_or_die(dest->moves,
                                             sizeof(IncrMove) * dest->capacity);
  }
  memcpy(dest->moves, src->moves, sizeof(IncrMove) * src->count);
  dest->count = src->count;
  dest->tile_mapping = src->tile_mapping;
}

// Compare IncrMoves by tiny_move for set-equivalence assertions.
static int compare_incr_moves_by_tiny_move(const void *a, const void *b) {
  const IncrMove *ma = (const IncrMove *)a;
  const IncrMove *mb = (const IncrMove *)b;
  if (ma->small_move.tiny_move < mb->small_move.tiny_move) {
    return -1;
  }
  if (ma->small_move.tiny_move > mb->small_move.tiny_move) {
    return 1;
  }
  return 0;
}

static int compare_small_moves_by_tiny_move(const void *a, const void *b) {
  const SmallMove *ma = (const SmallMove *)a;
  const SmallMove *mb = (const SmallMove *)b;
  if (ma->tiny_move < mb->tiny_move) {
    return -1;
  }
  if (ma->tiny_move > mb->tiny_move) {
    return 1;
  }
  return 0;
}

void incr_move_list_assert_matches_small_moves(const IncrMoveList *iml,
                                               const MoveList *ml) {
  assert(iml->count == ml->count);

  // Make sorted copies by tiny_move for comparison
  IncrMove *sorted_incr =
      (IncrMove *)malloc_or_die(sizeof(IncrMove) * iml->count);
  memcpy(sorted_incr, iml->moves, sizeof(IncrMove) * iml->count);
  qsort(sorted_incr, iml->count, sizeof(IncrMove),
        compare_incr_moves_by_tiny_move);

  SmallMove *sorted_sm =
      (SmallMove *)malloc_or_die(sizeof(SmallMove) * ml->count);
  for (int move_idx = 0; move_idx < ml->count; move_idx++) {
    sorted_sm[move_idx] = *ml->small_moves[move_idx];
  }
  qsort(sorted_sm, ml->count, sizeof(SmallMove),
        compare_small_moves_by_tiny_move);

  for (int move_idx = 0; move_idx < iml->count; move_idx++) {
    assert(sorted_incr[move_idx].small_move.tiny_move ==
           sorted_sm[move_idx].tiny_move);
    assert(sorted_incr[move_idx].small_move.metadata.score ==
           sorted_sm[move_idx].metadata.score);
  }

  free(sorted_incr);
  free(sorted_sm);
}

void incr_move_list_assert_subset_of(const IncrMoveList *subset,
                                     const IncrMoveList *superset) {
  if (subset->count == 0) {
    return;
  }

  // Sort copies of both lists by tiny_move
  IncrMove *sorted_sub =
      (IncrMove *)malloc_or_die(sizeof(IncrMove) * subset->count);
  memcpy(sorted_sub, subset->moves, sizeof(IncrMove) * subset->count);
  qsort(sorted_sub, subset->count, sizeof(IncrMove),
        compare_incr_moves_by_tiny_move);

  IncrMove *sorted_sup =
      (IncrMove *)malloc_or_die(sizeof(IncrMove) * superset->count);
  memcpy(sorted_sup, superset->moves, sizeof(IncrMove) * superset->count);
  qsort(sorted_sup, superset->count, sizeof(IncrMove),
        compare_incr_moves_by_tiny_move);

  // Walk both lists; every element in subset must appear in superset
  int sup_idx = 0;
  for (int sub_idx = 0; sub_idx < subset->count; sub_idx++) {
    uint64_t target = sorted_sub[sub_idx].small_move.tiny_move;
    while (sup_idx < superset->count &&
           sorted_sup[sup_idx].small_move.tiny_move < target) {
      sup_idx++;
    }
    if (sup_idx >= superset->count ||
        sorted_sup[sup_idx].small_move.tiny_move != target) {
      const SmallMove *bad_sm = &sorted_sub[sub_idx].small_move;
      bool vert = (target & 1) != 0;
      // small_move_to_move always uses row=bits6-10, col=bits1-5
      int orig_row = (int)((target & SMALL_MOVE_ROW_BITMASK) >> 6);
      int orig_col = (int)((target & SMALL_MOVE_COL_BITMASK) >> 1);
      (void)vert;
      log_fatal("incr_move_list_assert_subset_of: move 0x%llx "
                "(row=%d col=%d dir=%s score=%d tiles_played=%d "
                "play_length=%d) "
                "in invalidated list (%d moves) not found in full movegen "
                "result (%d moves)",
                (unsigned long long)target, orig_row, orig_col,
                vert ? "vert" : "horiz", small_move_get_score(bad_sm),
                small_move_get_tiles_played(bad_sm),
                small_move_get_play_length(bad_sm), subset->count,
                superset->count);
    }
    sup_idx++;
  }

  free(sorted_sub);
  free(sorted_sup);
}
