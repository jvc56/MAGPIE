#include "incr_move_gen.h"

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "move_gen.h"
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
  iml->generation_id = 0;
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
    MachineLetter ml =
        (tm & (1ULL << (12 + tile_idx))) ? BLANK_MACHINE_LETTER : tile;
    int local_idx = mapping->tile_to_index[ml];
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
  iml->board_tiles_played = -1; // Set by caller

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
      incr_move_set_tiles_and_anchor(im, tiles_used, 0, 0, 0);
    }
    iml->count++;
  }
}

// Get filled squares from a MoveUndo. Returns count written.
static int incr_get_filled_squares(const MoveUndo *undo, int *out_rows,
                                   int *out_cols) {
  if (undo->move_tiles_length == 0) {
    return 0;
  }
  int count = 0;
  const int row_start = undo->move_row_start;
  const int col_start = undo->move_col_start;
  const int dir = undo->move_dir;
  const uint16_t mask = undo->tiles_placed_mask;

  for (int pos_idx = 0; pos_idx < undo->move_tiles_length; pos_idx++) {
    if (!(mask & (1 << pos_idx))) {
      continue;
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

void incr_compute_dirty_lanes(const MoveUndo *undo1, const MoveUndo *undo2,
                              const Board *board, bool *dirty_lanes) {
  memset(dirty_lanes, 0, sizeof(bool) * 2 * BOARD_DIM);

  int filled_rows[RACK_SIZE * 2];
  int filled_cols[RACK_SIZE * 2];
  int num_filled = 0;
  if (undo1) {
    num_filled += incr_get_filled_squares(undo1, filled_rows, filled_cols);
  }
  if (undo2) {
    num_filled += incr_get_filled_squares(undo2, filled_rows + num_filled,
                                          filled_cols + num_filled);
  }

  for (int sq_idx = 0; sq_idx < num_filled; sq_idx++) {
    const int fr = filled_rows[sq_idx];
    const int fc = filled_cols[sq_idx];

    // Direct: the lanes containing this filled square
    dirty_lanes[0 * BOARD_DIM + fr] = true; // horizontal lane = row fr
    dirty_lanes[1 * BOARD_DIM + fc] = true; // vertical lane = column fc

    // Adjacent lanes for new anchor creation (±1)
    if (fr > 0) {
      dirty_lanes[0 * BOARD_DIM + fr - 1] = true;
    }
    if (fr < BOARD_DIM - 1) {
      dirty_lanes[0 * BOARD_DIM + fr + 1] = true;
    }
    if (fc > 0) {
      dirty_lanes[1 * BOARD_DIM + fc - 1] = true;
    }
    if (fc < BOARD_DIM - 1) {
      dirty_lanes[1 * BOARD_DIM + fc + 1] = true;
    }

    // Perpendicular word fragment: walk column fc up and down from fr
    // through contiguous tiles. Mark each row in the fragment PLUS the
    // empty boundary squares (cross-sets there change because the
    // perpendicular word above/below them extended).
    {
      int r = fr - 1;
      while (r >= 0 && !board_is_empty(board, r, fc)) {
        dirty_lanes[0 * BOARD_DIM + r] = true;
        r--;
      }
      if (r >= 0) {
        dirty_lanes[0 * BOARD_DIM + r] = true; // boundary empty square
      }
    }
    {
      int r = fr + 1;
      while (r < BOARD_DIM && !board_is_empty(board, r, fc)) {
        dirty_lanes[0 * BOARD_DIM + r] = true;
        r++;
      }
      if (r < BOARD_DIM) {
        dirty_lanes[0 * BOARD_DIM + r] = true; // boundary empty square
      }
    }

    // Walk row fr left and right, same pattern with boundary.
    {
      int c = fc - 1;
      while (c >= 0 && !board_is_empty(board, fr, c)) {
        dirty_lanes[1 * BOARD_DIM + c] = true;
        c--;
      }
      if (c >= 0) {
        dirty_lanes[1 * BOARD_DIM + c] = true;
      }
    }
    {
      int c = fc + 1;
      while (c < BOARD_DIM && !board_is_empty(board, fr, c)) {
        dirty_lanes[1 * BOARD_DIM + c] = true;
        c++;
      }
      if (c < BOARD_DIM) {
        dirty_lanes[1 * BOARD_DIM + c] = true;
      }
    }
  }
}

int incr_move_list_remove_dirty(IncrMoveList *iml, const bool *dirty_lanes,
                                const Rack *remaining_rack) {
  // Build rack_packed for feasibility check using existing tile mapping
  const IncrTileMapping *orig = &iml->tile_mapping;
  uint32_t rack_packed = 0;
  for (int idx = 0; idx < orig->num_unique_tiles; idx++) {
    uint8_t ml = orig->index_to_tile[idx];
    int count = rack_get_letter(remaining_rack, ml);
    rack_packed |= ((uint32_t)count << (idx * 3));
  }

  int write_idx = 0;
  for (int read_idx = 0; read_idx < iml->count; read_idx++) {
    const IncrMove *im = &iml->moves[read_idx];
    bool remove = false;

    if (small_move_is_pass(&im->small_move)) {
      // Always remove pass — regeneration will add a fresh one
      remove = true;
    } else {
      // Check if move's lane is dirty
      int lane = incr_move_get_lane(&im->small_move);
      if (dirty_lanes[lane]) {
        remove = true;
      }
      // Check rack feasibility (direction-independent)
      if (!remove) {
        uint32_t tiles_used = incr_move_get_tiles_used(im);
        if (!incr_move_tiles_feasible(tiles_used, rack_packed)) {
          remove = true;
        }
      }
    }

    if (!remove) {
      if (write_idx != read_idx) {
        iml->moves[write_idx] = iml->moves[read_idx];
      }
      write_idx++;
    }
  }

  int removed = iml->count - write_idx;
  iml->count = write_idx;
  iml->tile_mapping.rack_packed = rack_packed;
  return removed;
}

void incr_move_list_regenerate(IncrMoveList *iml,
                               const bool *dirty_lanes, Game *game,
                               MoveList *move_list, const KWG *pruned_kwg,
                               int thread_index) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = pruned_kwg,
      .thread_index = thread_index,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  MoveGen *gen = get_movegen(thread_index);
  gen_load_position(gen, &args);

  // Generate moves only for dirty lanes
  gen_record_scoring_plays_small_for_rows(gen, dirty_lanes);

  // Pre-allocate capacity for surviving + regenerated + pass
  int needed = iml->count + move_list->count + 1;
  if (needed > iml->capacity) {
    iml->capacity = needed;
    iml->moves = (IncrMove *)realloc_or_die(iml->moves,
                                            sizeof(IncrMove) * iml->capacity);
  }

  // Append regenerated moves — no dedup needed since surviving moves
  // are from clean lanes and regenerated moves are from dirty lanes.
  for (int move_idx = 0; move_idx < move_list->count; move_idx++) {
    const SmallMove *sm = move_list->small_moves[move_idx];
    IncrMove *im = &iml->moves[iml->count];
    im->small_move = *sm;
    uint32_t tiles_used =
        incr_move_compute_tiles_used(&iml->tile_mapping, sm);
    incr_move_set_tiles_and_anchor(im, tiles_used, 0, 0, 0);
    iml->count++;
  }

  // Add pass
  IncrMove *pass_im = &iml->moves[iml->count];
  small_move_set_as_pass(&pass_im->small_move);
  pass_im->tiles_and_anchor = 0;
  iml->count++;
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

// Compare IncrMoves by tiny_move for set-equivalence assertions and sorting.
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
  if (iml->count != ml->count) {
    log_fatal("incr_move_list_assert_matches_small_moves: count mismatch: "
              "incr=%d ml=%d",
              iml->count, ml->count);
  }

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
    if (sorted_incr[move_idx].small_move.tiny_move !=
        sorted_sm[move_idx].tiny_move) {
      log_fatal("incr_move_list_assert_matches: tiny_move mismatch at %d",
                move_idx);
    }
  }

  free(sorted_incr);
  free(sorted_sm);
}

void incr_move_list_assert_equal_sets(const IncrMoveList *a,
                                      const IncrMoveList *b) {
  if (a->count != b->count) {
    IncrMove *sa = (IncrMove *)malloc_or_die(sizeof(IncrMove) * a->count);
    memcpy(sa, a->moves, sizeof(IncrMove) * a->count);
    qsort(sa, a->count, sizeof(IncrMove), compare_incr_moves_by_tiny_move);
    IncrMove *sb = (IncrMove *)malloc_or_die(sizeof(IncrMove) * b->count);
    memcpy(sb, b->moves, sizeof(IncrMove) * b->count);
    qsort(sb, b->count, sizeof(IncrMove), compare_incr_moves_by_tiny_move);
    int ai = 0, bi = 0;
    while (ai < a->count && bi < b->count) {
      if (sa[ai].small_move.tiny_move < sb[bi].small_move.tiny_move) {
        log_warn("  EXTRA in incremental: 0x%llx score=%d tp=%d pl=%d",
                 (unsigned long long)sa[ai].small_move.tiny_move,
                 small_move_get_score(&sa[ai].small_move),
                 small_move_get_tiles_played(&sa[ai].small_move),
                 small_move_get_play_length(&sa[ai].small_move));
        ai++;
      } else if (sa[ai].small_move.tiny_move > sb[bi].small_move.tiny_move) {
        log_warn("  MISSING in incremental: 0x%llx score=%d tp=%d pl=%d",
                 (unsigned long long)sb[bi].small_move.tiny_move,
                 small_move_get_score(&sb[bi].small_move),
                 small_move_get_tiles_played(&sb[bi].small_move),
                 small_move_get_play_length(&sb[bi].small_move));
        bi++;
      } else {
        ai++;
        bi++;
      }
    }
    for (; ai < a->count; ai++) {
      log_warn("  EXTRA in incremental: 0x%llx",
               (unsigned long long)sa[ai].small_move.tiny_move);
    }
    for (; bi < b->count; bi++) {
      log_warn("  MISSING in incremental: 0x%llx",
               (unsigned long long)sb[bi].small_move.tiny_move);
    }
    free(sa);
    free(sb);
    log_fatal("incr_move_list_assert_equal_sets: count mismatch: "
              "incremental=%d full=%d",
              a->count, b->count);
  }

  IncrMove *sorted_a =
      (IncrMove *)malloc_or_die(sizeof(IncrMove) * a->count);
  memcpy(sorted_a, a->moves, sizeof(IncrMove) * a->count);
  qsort(sorted_a, a->count, sizeof(IncrMove), compare_incr_moves_by_tiny_move);

  IncrMove *sorted_b =
      (IncrMove *)malloc_or_die(sizeof(IncrMove) * b->count);
  memcpy(sorted_b, b->moves, sizeof(IncrMove) * b->count);
  qsort(sorted_b, b->count, sizeof(IncrMove), compare_incr_moves_by_tiny_move);

  for (int move_idx = 0; move_idx < a->count; move_idx++) {
    if (sorted_a[move_idx].small_move.tiny_move !=
        sorted_b[move_idx].small_move.tiny_move) {
      log_fatal("incr_move_list_assert_equal_sets: move mismatch at index %d: "
                "incremental=0x%llx full=0x%llx",
                move_idx,
                (unsigned long long)sorted_a[move_idx].small_move.tiny_move,
                (unsigned long long)sorted_b[move_idx].small_move.tiny_move);
    }
  }

  free(sorted_a);
  free(sorted_b);
}
