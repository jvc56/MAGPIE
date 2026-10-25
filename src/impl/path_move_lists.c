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
// count covers its used count. Every word is checked, with no early exit, so
// the check has no branch (clang subtracts the words as one vector).
static inline bool counts_subset(const PackedCounts *used,
                                 const PackedCounts *avail) {
  uint64_t borrow_free = PACKED_COUNTS_HIGH_BITS;
  for (int word_idx = 0; word_idx < PATH_MOVE_LISTS_COUNT_WORDS; word_idx++) {
    borrow_free &= (avail->words[word_idx] | PACKED_COUNTS_HIGH_BITS) -
                   used->words[word_idx];
  }
  return borrow_free == PACKED_COUNTS_HIGH_BITS;
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

// Per lane, the squares whose change can affect a play of that lane when
// `move`'s tiles land on `board` (as it stands before the move), written to
// `hits`; returns the lanes with any -- the lane-touch set.
//
// Derivation: a placed cell p affects exactly (a) plays whose span holds p or
// ends next to p in p's row or column, where the word now runs into p, and
// (b) plays placing a tile on the first empty cell reached from p in each of
// the four directions by walking through pre-existing tiles -- in-line, that
// cell's plays run through p's run and so end next to p or hold it; across,
// the cell whose cross-word p's run rewrites, which a play of the lane through
// that cell fills. So p and its two neighbours are hits in p's row and
// column, and each walk's end cell a hit in the lane across the walk.
// Every newly enabled play spans a hit too, since it must play through p or
// place on such a cell. A play spanning no hit keeps its squares, its ends,
// its cross-words and its anchors, so it carries over verbatim, score
// included.
static uint64_t move_lane_hits(const Board *board, const Move *move,
                               uint32_t *hits) {
  memset(hits, 0, PATH_MOVE_LISTS_NUM_LANES * sizeof(uint32_t));
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return 0;
  }
  static const int deltas[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  const uint32_t lane_squares =
      BOARD_DIM >= 32 ? ~(uint32_t)0 : ((uint32_t)1 << BOARD_DIM) - 1;
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
    // The square and its two neighbours along the lane.
    hits[row] |= (uint32_t)((((uint64_t)7 << col) >> 1) & lane_squares);
    hits[BOARD_DIM + col] |=
        (uint32_t)((((uint64_t)7 << row) >> 1) & lane_squares);
    touched |= lane_bit_row(row) | lane_bit_col(col);
    for (int delta_idx = 0; delta_idx < 4; delta_idx++) {
      int walk_row = row + deltas[delta_idx][0];
      int walk_col = col + deltas[delta_idx][1];
      while (board_is_position_in_bounds(walk_row, walk_col) &&
             board_is_nonempty_or_bricked(board, walk_row, walk_col)) {
        walk_row += deltas[delta_idx][0];
        walk_col += deltas[delta_idx][1];
      }
      if (!board_is_position_in_bounds(walk_row, walk_col)) {
        continue;
      }
      if (deltas[delta_idx][0] != 0) {
        // Walked along the column: the end's row sees its cross-word change.
        hits[walk_row] |= (uint32_t)1 << walk_col;
        touched |= lane_bit_row(walk_row);
      } else {
        hits[BOARD_DIM + walk_col] |= (uint32_t)1 << walk_row;
        touched |= lane_bit_col(walk_col);
      }
    }
  }
  return touched;
}

// The lane positions a small move spans, through tiles included.
static inline uint32_t small_move_span(const SmallMove *small_move) {
  const int start = small_move_is_vertical(small_move)
                        ? small_move_get_row_start(small_move)
                        : small_move_get_col_start(small_move);
  const int length = small_move_get_play_length(small_move);
  const uint32_t squares =
      length >= 32 ? ~(uint32_t)0 : ((uint32_t)1 << length) - 1;
  return squares << start;
}

// The anchor a play is generated from: the leftmost anchor in its span, the
// lowest bit of span & anchors, which a legal play always has.
static inline int span_anchor(uint32_t span, uint32_t anchors) {
  uint32_t spanned_anchors = span & anchors;
  assert(spanned_anchors != 0);
#if defined(__has_builtin) && __has_builtin(__builtin_ctz)
  return __builtin_ctz(spanned_anchors);
#else
  int index = 0;
  while ((spanned_anchors & 1U) == 0) {
    spanned_anchors >>= 1U;
    index++;
  }
  return index;
#endif
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

// The end of a list being built, held apart from its PathListSlot so the
// compiler keeps it in registers. A store of a play may alias the slot's int
// fields, so appending through the slot reloads its count and arrays after
// every play.
typedef struct SlotWriter {
  SmallMove *moves;
  PackedCounts *used;
  int count;
} SlotWriter;

static inline void writer_append(SlotWriter *writer,
                                 const SmallMove *small_move,
                                 const PackedCounts *used) {
  writer->moves[writer->count] = *small_move;
  writer->used[writer->count] = *used;
  writer->count++;
}

// Rebuild `slot` from `move_list`, a full generation in lane order ending in
// its pass, which is dropped.
static void slot_fill_from_move_list(PathListSlot *slot,
                                     const MoveList *move_list) {
  int count = move_list->count;
  assert(count > 0 && small_move_is_pass(move_list->small_moves[count - 1]));
  count--;
  slot_reserve(slot, count);
  SlotWriter writer = {slot->moves, slot->used, 0};
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
    writer_append(&writer, small_move, &used);
  }
  slot->count = writer.count;
  while (lane < PATH_MOVE_LISTS_NUM_LANES) {
    slot->lane_begin[++lane] = count;
  }
}

// Where a lane's merge stands after appending regenerated plays: the list's
// count, and the index and anchor of the next regenerated play (BOARD_DIM past
// the last).
typedef struct RegenFlush {
  int count;
  int regen_idx;
  int regen_anchor;
} RegenFlush;

// Appends the regenerated plays of `move_list` from `regen_idx`, whose anchor
// is below `anchor`, up to the first whose anchor is not, to the list `moves`
// and `used` of `count` plays. It runs once per run of regenerated plays, and
// is out of line for slot_merge_lane's loop over the parent's plays: inlined
// there, it left GCC too few registers, and the loop kept its pointers on the
// stack. It takes the list's arrays and count, not its SlotWriter, so that the
// writer stays in registers (see SlotWriter).
static __attribute__((noinline)) RegenFlush append_regenerated_below(
    SmallMove *moves, PackedCounts *used, int count, const MoveList *move_list,
    int regen_idx, int regen_end, int anchor, uint32_t anchors) {
  int regen_anchor;
  do {
    const SmallMove *regenerated = move_list->small_moves[regen_idx];
    moves[count] = *regenerated;
    used[count] = pack_move_used(regenerated);
    count++;
    regen_idx++;
    regen_anchor =
        regen_idx < regen_end
            ? span_anchor(small_move_span(move_list->small_moves[regen_idx]),
                          anchors)
            : BOARD_DIM;
  } while (regen_anchor < anchor);
  return (RegenFlush){count, regen_idx, regen_anchor};
}

// Appends lane `lane` of `out`: `parent`'s plays that span no square in `hit`
// and whose anchors (among `anchors`) are not in `generated`, filtered
// against `avail` iff `filter`, merged by anchor with the regenerated plays
// [regen_begin, regen_end) of `move_list`. Both sources list their plays by
// ascending anchor, so the merge is in generation order.
static void slot_merge_lane(SlotWriter *out, const PathListSlot *parent,
                            int lane, uint32_t hit, uint32_t anchors,
                            uint32_t generated, bool filter,
                            const PackedCounts *avail,
                            const MoveList *move_list, int regen_begin,
                            int regen_end) {
  int regen_idx = regen_begin;
  if (generated != anchors) {
    // The anchor of the next regenerated play, BOARD_DIM past the last.
    int regen_anchor =
        regen_idx < regen_end
            ? span_anchor(small_move_span(move_list->small_moves[regen_idx]),
                          anchors)
            : BOARD_DIM;
    const SmallMove *parent_moves = parent->moves;
    const PackedCounts *parent_used = parent->used;
    const int lane_end = parent->lane_begin[lane + 1];
    for (int move_idx = parent->lane_begin[lane]; move_idx < lane_end;
         move_idx++) {
      const SmallMove *small_move = &parent_moves[move_idx];
      const uint32_t span = small_move_span(small_move);
      if ((span & hit) != 0) {
        continue;
      }
      const int anchor = span_anchor(span, anchors);
      // The regenerated plays before this anchor go first even if this play
      // is dropped: the next play kept has an anchor no smaller, so the order
      // is the same.
      if (regen_anchor < anchor) {
        const RegenFlush flush = append_regenerated_below(
            out->moves, out->used, out->count, move_list, regen_idx, regen_end,
            anchor, anchors);
        out->count = flush.count;
        regen_idx = flush.regen_idx;
        regen_anchor = flush.regen_anchor;
      }
      // Without a branch, as slot_rebuild filters: the play is written to the
      // next free slot, which the reservation holds, and kept by counting it.
      const int keep = (((generated >> anchor) & 1) == 0) &
                       (!filter | counts_subset(&parent_used[move_idx], avail));
      out->moves[out->count] = *small_move;
      out->used[out->count] = parent_used[move_idx];
      out->count += keep;
    }
  }
  for (; regen_idx < regen_end; regen_idx++) {
    const SmallMove *regenerated = move_list->small_moves[regen_idx];
    const PackedCounts used = pack_move_used(regenerated);
    writer_append(out, regenerated, &used);
  }
}

// Derive `out` from `parent`: the `touched` lanes merge `parent`'s plays with
// `move_list`'s, whose lane L occupies [lane_end[L - 1], lane_end[L])
// (0 for L == 0), by `lists`'s lane scratch (see slot_merge_lane); the rest
// carry over from `parent`. Filtered against `avail` iff `filter`.
static void slot_rebuild(PathListSlot *out, const PathListSlot *parent,
                         uint64_t touched, bool filter,
                         const PackedCounts *avail, const MoveList *move_list,
                         const PathMoveLists *lists) {
  slot_reserve(out, parent->count + move_list->count);
  SlotWriter writer = {out->moves, out->used, 0};
  const SmallMove *parent_moves = parent->moves;
  const PackedCounts *parent_used = parent->used;
  int regen_begin = 0;
  for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
    out->lane_begin[lane] = writer.count;
    const int begin = parent->lane_begin[lane];
    const int end = parent->lane_begin[lane + 1];
    if ((touched >> lane) & 1) {
      slot_merge_lane(&writer, parent, lane, lists->lane_hits[lane],
                      lists->lane_anchors[lane], lists->lane_generated[lane],
                      filter, avail, move_list, regen_begin,
                      lists->lane_end[lane]);
      regen_begin = lists->lane_end[lane];
    } else if (filter) {
      // Without a branch: every play is written to the next free slot, which
      // the reservation holds, and kept by counting it.
      for (int move_idx = begin; move_idx < end; move_idx++) {
        writer.moves[writer.count] = parent_moves[move_idx];
        writer.used[writer.count] = parent_used[move_idx];
        writer.count += counts_subset(&parent_used[move_idx], avail) ? 1 : 0;
      }
    } else if (end > begin) {
      memcpy(writer.moves + writer.count, parent_moves + begin,
             (size_t)(end - begin) * sizeof(SmallMove));
      memcpy(writer.used + writer.count, parent_used + begin,
             (size_t)(end - begin) * sizeof(PackedCounts));
      writer.count += end - begin;
    }
  }
  out->count = writer.count;
  out->lane_begin[PATH_MOVE_LISTS_NUM_LANES] = writer.count;
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

void path_move_lists_set_current(PathMoveLists *lists,
                                 const MoveList *move_list) {
  assert(lists->length >= 1);
  slot_fill_from_move_list(&lists->slots[lists->length], move_list);
  lists->slot_stamps[lists->length] = lists->stamps[lists->length - 1];
}

void path_move_lists_push(PathMoveLists *lists, const Board *board,
                          const Move *move) {
  assert(lists->length < PATH_MOVE_LISTS_MAX_PATH);
  lists->masks[lists->length] =
      move_lane_hits(board, move, lists->hits[lists->length]);
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
  assert(length <= 2 || path_move_lists_slot_is_current(lists, length - 2));
  const PathListSlot *parent =
      length <= 2 ? &lists->roots[2 - length] : &lists->slots[length - 2];
  uint64_t touched = lists->masks[length - 1];
  bool filter = false;
  if (length >= 2) {
    touched |= lists->masks[length - 2];
    filter = lists->played[length - 2];
  }
  if (touched != 0) {
    for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
      lists->lane_hits[lane] = lists->hits[length - 1][lane];
      if (length >= 2) {
        lists->lane_hits[lane] |= lists->hits[length - 2][lane];
      }
    }
    generate_small_moves_in_lanes(args, touched, lists->lane_hits,
                                  lists->lane_end, lists->lane_anchors,
                                  lists->lane_generated);
  } else {
    // Two passes: nothing to regenerate, and the scratch list is unused.
    memset(lists->lane_end, 0, sizeof(lists->lane_end));
  }
  const Game *game = args->game;
  const Rack *rack = player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game)));
  const PackedCounts avail = pack_rack(rack);
  PathListSlot *out = &lists->slots[length];
  slot_rebuild(out, parent, touched, filter, &avail, args->move_list, lists);
  lists->slot_stamps[length] = lists->stamps[length - 1];
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
// the current leaf's own move touched, so they stand as they did after the
// side's own previous move. Lanes that move also left alone carry over from
// the side's list two moves up; in the others, only the anchors that can reach
// a square the move changed are regenerated on the current board, and the
// other anchors' plays carry over too.
static void fill_sibling_lanes(PathMoveLists *lists,
                               const MoveGenArgs *best_small_args,
                               uint64_t missing, int side_to_move_tiles) {
  const int length = lists->length;
  const uint64_t previous_move = length >= 2 ? lists->masks[length - 2] : 0;
  const uint64_t regenerated = missing & previous_move;
  for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
    if (((missing >> lane) & 1) != 0) {
      lists->sibling_lanes[length][lane].has_whole_rack = false;
      lists->sibling_lanes[length][lane].has_partial = false;
      lists->lane_hits[lane] = 0;
      lists->lane_generated[lane] = 0;
      lists->lane_anchors[lane] = 0;
    }
  }
  if (regenerated != 0) {
    for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
      lists->lane_hits[lane] = lists->hits[length - 2][lane];
    }
    MoveGenArgs all_small_args = *best_small_args;
    all_small_args.move_record_type = MOVE_RECORD_ALL_SMALL;
    generate_small_moves_in_lanes(&all_small_args, regenerated,
                                  lists->lane_hits, lists->lane_end,
                                  lists->lane_anchors, lists->lane_generated);
    const MoveList *move_list = best_small_args->move_list;
    int begin = 0;
    for (int lane = 0; lane < PATH_MOVE_LISTS_NUM_LANES; lane++) {
      const int end = lists->lane_end[lane];
      if (((regenerated >> lane) & 1) != 0) {
        for (int move_idx = begin; move_idx < end; move_idx++) {
          lane_best_consider(&lists->sibling_lanes[length][lane],
                             move_list->small_moves[move_idx],
                             side_to_move_tiles);
        }
      }
      begin = end;
    }
  }
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
    if (((missing >> lane) & 1) == 0) {
      continue;
    }
    // A lane the move left alone has no hits, so all of its plays carry.
    const uint32_t hit = lists->lane_hits[lane];
    const uint32_t anchors = lists->lane_anchors[lane];
    const uint32_t generated = lists->lane_generated[lane];
    if (hit != 0 && generated == anchors) {
      continue;
    }
    LaneBestPlays *best = &lists->sibling_lanes[length][lane];
    for (int move_idx = parent->lane_begin[lane];
         move_idx < parent->lane_begin[lane + 1]; move_idx++) {
      if (hit != 0) {
        const uint32_t span = small_move_span(&parent->moves[move_idx]);
        if ((span & hit) != 0 ||
            ((generated >> span_anchor(span, anchors)) & 1) != 0) {
          continue;
        }
      }
      if (filter && !counts_subset(&parent->used[move_idx], &avail)) {
        continue;
      }
      lane_best_consider(best, &parent->moves[move_idx], side_to_move_tiles);
    }
  }
}

bool path_move_lists_best_play(PathMoveLists *lists,
                               const MoveGenArgs *best_small_args,
                               SmallMove *out_best) {
  const int length = lists->length;
  assert(length >= 1);
  assert(length <= 2 || path_move_lists_slot_is_current(lists, length - 2));
  assert(best_small_args->move_record_type == MOVE_RECORD_BEST_SMALL);
  assert(best_small_args->best_small_canonical_ties);
  const Game *game = best_small_args->game;
  const int side_to_move_tiles = rack_get_total_letters(player_get_rack(
      game_get_player(game, game_get_player_on_turn_index(game))));
  // The prefix shared by this leaf's siblings ends with the side's own
  // previous move (or at the root).
  const uint64_t prefix_stamp =
      length >= 2 ? lists->stamps[length - 2] : lists->root_stamp;
  if (lists->sibling_stamp[length] != prefix_stamp) {
    lists->sibling_stamp[length] = prefix_stamp;
    lists->sibling_valid[length] = 0;
  }
  const uint64_t own_lanes = lists->masks[length - 1];
  const uint64_t shared_lanes = all_lanes_mask() & ~own_lanes;
  const uint64_t missing = shared_lanes & ~lists->sibling_valid[length];
  if (missing != 0) {
    fill_sibling_lanes(lists, best_small_args, missing, side_to_move_tiles);
    lists->sibling_valid[length] |= missing;
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
    const LaneBestPlays *best = &lists->sibling_lanes[length][lane];
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
