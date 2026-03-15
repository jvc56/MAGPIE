#ifndef INCR_MOVE_GEN_H
#define INCR_MOVE_GEN_H

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/move.h"
#include "../ent/move_undo.h"
#include "../ent/rack.h"
#include <stdbool.h>
#include <stdint.h>

// IncrMove: compact move representation for incremental movegen.
// Wraps SmallMove with:
//   - tiles_used_packed: 7 x 3-bit counts (21 bits) indexing into a
//     per-ply tile mapping from unique rack tile types to local indices
//   - anchor_row/col/dir: which anchor generated this move (11 bits)
// Total extra: 32 bits (one uint32_t), for 20 bytes per IncrMove.
typedef struct IncrMove {
  SmallMove small_move;
  uint32_t tiles_and_anchor;
} IncrMove;

// Packing layout for tiles_and_anchor:
//   bits  0-20: tiles_used (7 x 3-bit counts, index 0 in bits 0-2, etc.)
//   bits 21-25: anchor_row (5 bits, supports BOARD_DIM up to 31)
//   bits 26-30: anchor_col (5 bits)
//   bit     31: anchor_dir (0=horizontal, 1=vertical)
#define INCR_TILES_MASK 0x001FFFFFU
#define INCR_ANCHOR_ROW_SHIFT 21
#define INCR_ANCHOR_COL_SHIFT 26
#define INCR_ANCHOR_DIR_SHIFT 31

static inline uint32_t incr_move_get_tiles_used(const IncrMove *im) {
  return im->tiles_and_anchor & INCR_TILES_MASK;
}

static inline int incr_move_get_anchor_row(const IncrMove *im) {
  return (int)((im->tiles_and_anchor >> INCR_ANCHOR_ROW_SHIFT) & 0x1F);
}

static inline int incr_move_get_anchor_col(const IncrMove *im) {
  return (int)((im->tiles_and_anchor >> INCR_ANCHOR_COL_SHIFT) & 0x1F);
}

static inline int incr_move_get_anchor_dir(const IncrMove *im) {
  return (int)(im->tiles_and_anchor >> INCR_ANCHOR_DIR_SHIFT);
}

static inline void incr_move_set_tiles_and_anchor(IncrMove *im,
                                                   uint32_t tiles_used,
                                                   int anchor_row,
                                                   int anchor_col,
                                                   int anchor_dir) {
  im->tiles_and_anchor =
      (tiles_used & INCR_TILES_MASK) |
      ((uint32_t)anchor_row << INCR_ANCHOR_ROW_SHIFT) |
      ((uint32_t)anchor_col << INCR_ANCHOR_COL_SHIFT) |
      ((uint32_t)anchor_dir << INCR_ANCHOR_DIR_SHIFT);
}

// Per-ply tile mapping: maps the rack's unique tile types to local indices
// 0..6. Shared across all IncrMoves at a given ply.
typedef struct IncrTileMapping {
  uint8_t tile_to_index[MAX_ALPHABET_SIZE]; // ml -> local index (0-6)
  uint8_t index_to_tile[RACK_SIZE + 1];    // local index -> ml
  int num_unique_tiles;
  uint32_t rack_packed; // packed rack counts (7 x 3-bit), same layout as
                        // tiles_used
} IncrTileMapping;

// IncrMoveList: sorted array of IncrMoves for one player at one ply.
typedef struct IncrMoveList {
  IncrMove *moves;
  int count;
  int capacity;
  IncrTileMapping tile_mapping;
  int player_index;       // which player's moves these are
  int rack_total_letters;  // rack size when generated (for staleness check)
  int board_tiles_played;  // board_get_tiles_played when generated
  uint64_t generation_id;  // monotonic counter for staleness detection
} IncrMoveList;

// Create/destroy
IncrMoveList *incr_move_list_create(int capacity);
void incr_move_list_destroy(IncrMoveList *iml);
void incr_move_list_reset(IncrMoveList *iml);

// Set up the tile mapping from a rack (call once per ply, before adding moves)
void incr_move_list_setup_tile_mapping(IncrMoveList *iml, const Rack *rack);

// Compute the packed tiles_used for a SmallMove given the current tile mapping
uint32_t incr_move_compute_tiles_used(const IncrTileMapping *mapping,
                                      const SmallMove *sm);

// Populate IncrMoveList from a MoveList of SmallMoves (full generation)
void incr_move_list_populate_from_small_moves(IncrMoveList *iml,
                                              const MoveList *ml,
                                              const Rack *rack,
                                              int player_index);

// Copy src into dest, growing dest's capacity if needed.
void incr_move_list_copy_into(IncrMoveList *dest, const IncrMoveList *src);

// Compute dirty lanes from two MoveUndos. A lane is (dir, movegen_row).
// dirty_lanes[dir * BOARD_DIM + row] is set to true for each dirty lane.
// Dirty lanes include: direct lanes containing filled squares, perpendicular
// word fragment lanes (cross-set propagation), and ±1 adjacent lanes (new
// anchors). Also removes rack-infeasible moves from iml (these are
// direction-independent so handled separately from lane logic).
void incr_compute_dirty_lanes(const MoveUndo *undo1, const MoveUndo *undo2,
                              const Board *board, bool *dirty_lanes);

// Remove all moves from dirty lanes AND rack-infeasible moves.
// Returns number of moves removed.
int incr_move_list_remove_dirty(IncrMoveList *iml, const bool *dirty_lanes,
                                const Rack *remaining_rack);

// Regenerate moves for dirty lanes. Runs movegen only for those lanes,
// appends results to iml. Also ensures pass is present.
// move_list is used as scratch space for movegen output.
// No dedup needed: surviving moves are from clean lanes, regenerated
// moves are from dirty lanes — zero overlap.
void incr_move_list_regenerate(IncrMoveList *iml,
                               const bool *dirty_lanes, Game *game,
                               MoveList *move_list, const KWG *pruned_kwg,
                               int thread_index);

// Assert that the IncrMoveList contains the same moves as the MoveList
// (set equivalence, ignoring order). For debugging/validation.
void incr_move_list_assert_matches_small_moves(const IncrMoveList *iml,
                                               const MoveList *ml);

// Assert that two IncrMoveLists contain exactly the same set of moves
// (by tiny_move, ignoring order). Used to validate that incremental
// invalidation + regeneration produces the same result as full movegen.
void incr_move_list_assert_equal_sets(const IncrMoveList *a,
                                      const IncrMoveList *b);

// Extract a move's lane index for the dirty_lanes array.
// Returns dir * BOARD_DIM + movegen_row.
static inline int incr_move_get_lane(const SmallMove *sm) {
  const uint64_t tm = sm->tiny_move;
  const int dir = (int)(tm & 1);
  int movegen_row;
  if (dir == 0) {
    // Horizontal: movegen row = actual row = bits 6-10
    movegen_row = (int)((tm & SMALL_MOVE_ROW_BITMASK) >> 6);
  } else {
    // Vertical: movegen row = actual column = bits 1-5
    movegen_row = (int)((tm & SMALL_MOVE_COL_BITMASK) >> 1);
  }
  return dir * BOARD_DIM + movegen_row;
}

// Check if a move's tiles are feasible with the given packed rack counts.
// Both tiles_used and rack_packed use 7 x 3-bit fields at indices 0..6.
// A move is feasible if, for every index, used_count <= avail_count.
static inline bool incr_move_tiles_feasible(uint32_t tiles_used,
                                            uint32_t rack_packed) {
  // Quick check: if all used bits are covered by rack bits, we're good.
  // Full field-by-field check otherwise.
  if (tiles_used == 0) {
    return true;
  }
  for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
    int used = (int)((tiles_used >> (tile_idx * 3)) & 7);
    int avail = (int)((rack_packed >> (tile_idx * 3)) & 7);
    if (used > avail) {
      return false;
    }
  }
  return true;
}

#endif
