#include "path_move_lists.h"

#include "../def/board_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "move_gen.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static_assert(PATH_MOVE_LISTS_NUM_LANES <= 64,
              "lane masks are 64-bit; BOARD_DIM must be at most 32");
static_assert(RACK_SIZE < 8, "packed counts hold at most 7 of a tile type");

enum {
  PATH_MOVE_LISTS_INITIAL_CAPACITY = 1024,
};

// High bit of every 4-bit field: the borrow sentinel of counts_subset.
static const uint64_t PACKED_COUNTS_HIGH_BITS = 0x8888888888888888ULL;

static inline void packed_counts_add(PackedCounts *counts, MachineLetter ml) {
  counts->words[ml / 16] += (uint64_t)1 << (4 * (ml % 16));
}

static PackedCounts pack_rack(const Rack *rack) {
  PackedCounts counts;
  memset(&counts, 0, sizeof(counts));
  const int dist_size = rack_get_dist_size(rack);
  for (int ml = 0; ml < dist_size; ml++) {
    const int number_of_ml = rack_get_letter(rack, (MachineLetter)ml);
    for (int copy_idx = 0; copy_idx < number_of_ml; copy_idx++) {
      packed_counts_add(&counts, (MachineLetter)ml);
    }
  }
  return counts;
}

// The rack tiles a small move places: a blank-designated tile counts as a
// blank.
static PackedCounts pack_move_used(const SmallMove *small_move) {
  PackedCounts counts;
  memset(&counts, 0, sizeof(counts));
  for (int tile_idx = 0; tile_idx < SMALL_MOVE_MAX_TILES; tile_idx++) {
    const MachineLetter tile = small_move_get_tile(small_move, tile_idx);
    if (tile == 0) {
      break;
    }
    packed_counts_add(&counts, small_move_tile_is_blank(small_move, tile_idx)
                                   ? BLANK_MACHINE_LETTER
                                   : tile);
  }
  return counts;
}

// True iff `used` is a sub-multiset of `avail`: per 4-bit field, seeding the
// high bit makes the subtraction borrow-free exactly when the field's avail
// count covers its used count.
static inline bool counts_subset(const PackedCounts *used,
                                 const PackedCounts *avail) {
  for (int word_idx = 0; word_idx < PATH_MOVE_LISTS_COUNT_WORDS; word_idx++) {
    const uint64_t diff = (avail->words[word_idx] | PACKED_COUNTS_HIGH_BITS) -
                          used->words[word_idx];
    if ((diff & PACKED_COUNTS_HIGH_BITS) != PACKED_COUNTS_HIGH_BITS) {
      return false;
    }
  }
  return true;
}

static inline uint64_t lane_bit_row(int row) { return (uint64_t)1 << row; }

static inline uint64_t lane_bit_col(int col) {
  return (uint64_t)1 << (BOARD_DIM + col);
}

// The lane of a small move in the generation order: horizontal plays live in
// their row's lane, vertical plays in their column's lane after all rows.
// tiny_move keeps the true board coordinates of the first square (bits 6-10
// the row, bits 1-5 the column) for both directions.
static inline int lane_of_small_move(const SmallMove *small_move) {
  return small_move_is_vertical(small_move)
             ? BOARD_DIM + small_move_get_col_start(small_move)
             : small_move_get_row_start(small_move);
}

// The lanes whose legal-play sets can change when `move`'s tiles land on
// `board` (as it stands before the move) -- the lane-touch set.
//
// Derivation: a placed cell p affects exactly (a) plays placing a tile on p
// and (b) plays placing a tile on the first empty cell reached from p in each
// of the four directions by walking through pre-existing tiles -- in-line,
// the cell a word extends through p's run from; perpendicular, the cell whose
// cross-word p's run rewrites. Marking the rows and columns of p and of those
// (at most four) cells therefore covers every affected play, and every newly
// enabled play too, since it must place on such a cell or play through p.
// Anchor, cross-set, extension-set, and single-tile-dedup state in unmarked
// lanes is untouched, so their plays carry over verbatim, scores included.
static uint64_t move_lane_influence(const Board *board, const Move *move) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return 0;
  }
  static const int deltas[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int tiles_length = move_get_tiles_length(move);
  uint64_t touched = 0;
  for (int tile_idx = 0; tile_idx < tiles_length; tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int row = vertical ? row_start + tile_idx : row_start;
    const int col = vertical ? col_start : col_start + tile_idx;
    touched |= lane_bit_row(row) | lane_bit_col(col);
    for (int delta_idx = 0; delta_idx < 4; delta_idx++) {
      int walk_row = row + deltas[delta_idx][0];
      int walk_col = col + deltas[delta_idx][1];
      while (board_is_position_in_bounds(walk_row, walk_col) &&
             board_is_nonempty_or_bricked(board, walk_row, walk_col)) {
        walk_row += deltas[delta_idx][0];
        walk_col += deltas[delta_idx][1];
      }
      if (board_is_position_in_bounds(walk_row, walk_col)) {
        touched |= lane_bit_row(walk_row) | lane_bit_col(walk_col);
      }
    }
  }
  return touched;
}

static void slot_reserve(PathListSlot *slot, int needed) {
  if (slot->capacity >= needed) {
    return;
  }
  int capacity = slot->capacity;
  if (capacity < PATH_MOVE_LISTS_INITIAL_CAPACITY) {
    capacity = PATH_MOVE_LISTS_INITIAL_CAPACITY;
  }
  while (capacity < needed) {
    capacity *= 2;
  }
  slot->moves =
      realloc_or_die(slot->moves, (size_t)capacity * sizeof(SmallMove));
  slot->used =
      realloc_or_die(slot->used, (size_t)capacity * sizeof(PackedCounts));
  slot->capacity = capacity;
}

static inline void slot_append(PathListSlot *slot, const SmallMove *small_move,
                               const PackedCounts *used) {
  slot->moves[slot->count] = *small_move;
  slot->used[slot->count] = *used;
  slot->count++;
}

// Rebuild `slot` from `move_list`, a full generation in lane order ending in
// its pass, which is dropped.
static void slot_fill_from_move_list(PathListSlot *slot,
                                     const MoveList *move_list) {
  int count = move_list->count;
  assert(count > 0 && small_move_is_pass(move_list->small_moves[count - 1]));
  count--;
  slot_reserve(slot, count);
  slot->count = 0;
  int lane = 0;
  slot->lane_begin[0] = 0;
  for (int move_idx = 0; move_idx < count; move_idx++) {
    const SmallMove *small_move = move_list->small_moves[move_idx];
    const int move_lane = lane_of_small_move(small_move);
    assert(move_lane >= lane);
    while (lane < move_lane) {
      slot->lane_begin[++lane] = move_idx;
    }
    const PackedCounts used = pack_move_used(small_move);
    slot_append(slot, small_move, &used);
  }
  while (lane < PATH_MOVE_LISTS_NUM_LANES) {
    slot->lane_begin[++lane] = count;
  }
}

// Derive `out` from `parent`: the `touched` lanes come from `move_list`, whose
// lane L occupies [lane_end[L - 1], lane_end[L]) (0 for L == 0); the rest
// carry over from `parent`, filtered against `avail` iff `filter`.
static void slot_rebuild(PathListSlot *out, const PathListSlot *parent,
                         uint64_t touched, bool filter,
                         const PackedCounts *avail, const MoveList *move_list,
                         const int *lane_end) {
  slot_reserve(out, parent->count + move_list->count);
  out->count = 0;
  int regen_begin = 0;
  for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
    out->lane_begin[lane] = out->count;
    if ((touched >> lane) & 1) {
      for (int move_idx = regen_begin; move_idx < lane_end[lane]; move_idx++) {
        const SmallMove *small_move = move_list->small_moves[move_idx];
        const PackedCounts used = pack_move_used(small_move);
        slot_append(out, small_move, &used);
      }
      regen_begin = lane_end[lane];
    } else if (filter) {
      for (int move_idx = parent->lane_begin[lane];
           move_idx < parent->lane_begin[lane + 1]; move_idx++) {
        if (!counts_subset(&parent->used[move_idx], avail)) {
          continue;
        }
        slot_append(out, &parent->moves[move_idx], &parent->used[move_idx]);
      }
    } else {
      const int begin = parent->lane_begin[lane];
      const int lane_count = parent->lane_begin[lane + 1] - begin;
      if (lane_count > 0) {
        memcpy(out->moves + out->count, parent->moves + begin,
               (size_t)lane_count * sizeof(SmallMove));
        memcpy(out->used + out->count, parent->used + begin,
               (size_t)lane_count * sizeof(PackedCounts));
      }
      out->count += lane_count;
    }
  }
  out->lane_begin[PATH_MOVE_LISTS_NUM_LANES] = out->count;
}

PathMoveLists *path_move_lists_create(void) {
  PathMoveLists *lists = calloc_or_die(1, sizeof(PathMoveLists));
  return lists;
}

static void slot_free(PathListSlot *slot) {
  free(slot->moves);
  free(slot->used);
}

void path_move_lists_destroy(PathMoveLists *lists) {
  if (!lists) {
    return;
  }
  slot_free(&lists->roots[0]);
  slot_free(&lists->roots[1]);
  for (int slot_idx = 0; slot_idx < PATH_MOVE_LISTS_MAX_PATH; slot_idx++) {
    slot_free(&lists->slots[slot_idx]);
  }
  free(lists);
}

void path_move_lists_reset(PathMoveLists *lists) { lists->length = 0; }

void path_move_lists_set_root(PathMoveLists *lists, int side,
                              const MoveList *move_list) {
  slot_fill_from_move_list(&lists->roots[side], move_list);
  lists->root_stamp = ++lists->next_stamp;
}

void path_move_lists_push(PathMoveLists *lists, const Board *board,
                          const Move *move) {
  assert(lists->length < PATH_MOVE_LISTS_MAX_PATH);
  lists->masks[lists->length] = move_lane_influence(board, move);
  lists->played[lists->length] =
      move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE;
  lists->stamps[lists->length] = ++lists->next_stamp;
  lists->length++;
}

void path_move_lists_pop(PathMoveLists *lists) {
  assert(lists->length > 0);
  lists->length--;
}

int path_move_lists_moves_at(PathMoveLists *lists, const MoveGenArgs *args,
                             const SmallMove **out_moves) {
  const int length = lists->length;
  assert(length >= 1);
  // The parent is the same side's list two moves up; at lengths 1 and 2 that
  // is a root list.
  const PathListSlot *parent =
      length <= 2 ? &lists->roots[2 - length] : &lists->slots[length - 2];
  uint64_t touched = lists->masks[length - 1];
  bool filter = false;
  if (length >= 2) {
    touched |= lists->masks[length - 2];
    filter = lists->played[length - 2];
  }
  if (touched != 0) {
    generate_small_moves_in_lanes(args, touched, lists->lane_end);
  } else {
    // Two passes: nothing to regenerate, and the scratch list is unused.
    memset(lists->lane_end, 0, sizeof(lists->lane_end));
  }
  const Game *game = args->game;
  const Rack *rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  const PackedCounts avail = pack_rack(rack);
  PathListSlot *out = &lists->slots[length];
  slot_rebuild(out, parent, touched, filter, &avail, args->move_list,
               lists->lane_end);
  *out_moves = out->moves;
  return out->count;
}

// The running argmax of path_move_lists_best_play. A play outranks the current
// best when its adjusted score is higher, or equal with a larger tiny_move.
typedef struct BestScan {
  int best_adjusted;
  uint64_t best_tiny_move;
  const SmallMove *best_play;
  int go_out_bonus;
  int side_to_move_tiles;
} BestScan;

static inline void best_scan_consider(BestScan *scan,
                                      const SmallMove *small_move) {
  int adjusted = small_move_get_score(small_move);
  if (small_move_get_tiles_played(small_move) == scan->side_to_move_tiles) {
    adjusted += scan->go_out_bonus;
  }
  if (adjusted > scan->best_adjusted ||
      (adjusted == scan->best_adjusted &&
       small_move->tiny_move > scan->best_tiny_move)) {
    scan->best_adjusted = adjusted;
    scan->best_tiny_move = small_move->tiny_move;
    scan->best_play = small_move;
  }
}

// Every lane, rows then columns.
static inline uint64_t all_lanes_mask(void) {
  return PATH_MOVE_LISTS_NUM_LANES >= 64
             ? ~(uint64_t)0
             : ((uint64_t)1 << PATH_MOVE_LISTS_NUM_LANES) - 1;
}

// Keeps `small_move` if it is its lane's best whole-rack or best partial play
// by score, ties going to the larger tiny_move.
static inline void lane_best_consider(LaneBestPlays *best,
                                      const SmallMove *small_move,
                                      int side_to_move_tiles) {
  const bool whole_rack =
      small_move_get_tiles_played(small_move) == side_to_move_tiles;
  SmallMove *kept = whole_rack ? &best->whole_rack : &best->partial;
  bool *has_kept = whole_rack ? &best->has_whole_rack : &best->has_partial;
  if (!*has_kept ||
      small_move_get_score(small_move) > small_move_get_score(kept) ||
      (small_move_get_score(small_move) == small_move_get_score(kept) &&
       small_move->tiny_move > kept->tiny_move)) {
    *kept = *small_move;
    *has_kept = true;
  }
}

// Finds the sibling-shared best plays of the lanes in `missing`, none of which
// the current leaf's own move touched. Lanes the side's own previous move also
// left alone carry over from its list two moves up; the others are
// regenerated on the current board, which matches the parent's there.
static void fill_sibling_lanes(PathMoveLists *lists,
                               const MoveGenArgs *best_small_args,
                               uint64_t missing, int side_to_move_tiles) {
  const int length = lists->length;
  const uint64_t previous_move = length >= 2 ? lists->masks[length - 2] : 0;
  const uint64_t carried = missing & ~previous_move;
  const uint64_t regenerated = missing & previous_move;
  for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
    if (((missing >> lane) & 1) != 0) {
      lists->sibling_lanes[lane].has_whole_rack = false;
      lists->sibling_lanes[lane].has_partial = false;
    }
  }
  if (carried != 0) {
    const PathListSlot *parent =
        length <= 2 ? &lists->roots[2 - length] : &lists->slots[length - 2];
    const bool filter = length >= 2 && lists->played[length - 2];
    PackedCounts avail;
    if (filter) {
      const Game *game = best_small_args->game;
      avail = pack_rack(player_get_rack(
          game_get_player(game, game_get_player_on_turn_index(game))));
    }
    for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
      if (((carried >> lane) & 1) == 0) {
        continue;
      }
      LaneBestPlays *best = &lists->sibling_lanes[lane];
      for (int move_idx = parent->lane_begin[lane];
           move_idx < parent->lane_begin[lane + 1]; move_idx++) {
        if (filter && !counts_subset(&parent->used[move_idx], &avail)) {
          continue;
        }
        lane_best_consider(best, &parent->moves[move_idx], side_to_move_tiles);
      }
    }
  }
  if (regenerated != 0) {
    MoveGenArgs all_small_args = *best_small_args;
    all_small_args.move_record_type = MOVE_RECORD_ALL_SMALL;
    generate_small_moves_in_lanes(&all_small_args, regenerated,
                                  lists->lane_end);
    const MoveList *move_list = best_small_args->move_list;
    int begin = 0;
    for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
      const int end = lists->lane_end[lane];
      if (((regenerated >> lane) & 1) != 0) {
        for (int move_idx = begin; move_idx < end; move_idx++) {
          lane_best_consider(&lists->sibling_lanes[lane],
                             move_list->small_moves[move_idx],
                             side_to_move_tiles);
        }
      }
      begin = end;
    }
  }
}

bool path_move_lists_best_play(PathMoveLists *lists,
                               const MoveGenArgs *best_small_args,
                               SmallMove *out_best) {
  const int length = lists->length;
  assert(length >= 1);
  assert(best_small_args->move_record_type == MOVE_RECORD_BEST_SMALL);
  const Game *game = best_small_args->game;
  const int side_to_move_tiles = rack_get_total_letters(player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game))));
  // The prefix shared by this leaf's siblings ends with the side's own
  // previous move (or at the root).
  const uint64_t prefix_stamp =
      length >= 2 ? lists->stamps[length - 2] : lists->root_stamp;
  if (lists->sibling_length != length || lists->sibling_stamp != prefix_stamp) {
    lists->sibling_length = length;
    lists->sibling_stamp = prefix_stamp;
    lists->sibling_valid = 0;
  }
  const uint64_t own_lanes = lists->masks[length - 1];
  const uint64_t shared_lanes = all_lanes_mask() & ~own_lanes;
  const uint64_t missing = shared_lanes & ~lists->sibling_valid;
  if (missing != 0) {
    fill_sibling_lanes(lists, best_small_args, missing, side_to_move_tiles);
    lists->sibling_valid |= missing;
  }

  BestScan scan = {
      .best_adjusted = INT32_MIN,
      .best_tiny_move = 0,
      .best_play = NULL,
      .go_out_bonus = equity_to_int(best_small_args->best_small_out_bonus),
      .side_to_move_tiles = side_to_move_tiles,
  };
  for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
    if (((shared_lanes >> lane) & 1) == 0) {
      continue;
    }
    const LaneBestPlays *best = &lists->sibling_lanes[lane];
    if (best->has_whole_rack) {
      best_scan_consider(&scan, &best->whole_rack);
    }
    if (best->has_partial) {
      best_scan_consider(&scan, &best->partial);
    }
  }
  if (own_lanes == 0) {
    if (scan.best_play == NULL) {
      return false;
    }
    *out_best = *scan.best_play;
    return true;
  }
  // The shared lanes' best is the incumbent the own lanes' anchors are pruned
  // against.
  if (!generate_best_small_move_in_lanes(
          best_small_args, own_lanes, scan.best_play,
          scan.best_play == NULL ? 0 : int_to_equity(scan.best_adjusted))) {
    return false;
  }
  *out_best = *best_small_args->move_list->small_moves[0];
  return true;
}
