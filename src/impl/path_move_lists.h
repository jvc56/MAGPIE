#ifndef PATH_MOVE_LISTS_H
#define PATH_MOVE_LISTS_H

#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/board.h"
#include "../ent/move.h"
#include "move_gen.h"
#include <stdbool.h>
#include <stdint.h>

enum {
  // The BOARD_DIM horizontal rows followed by the BOARD_DIM vertical columns,
  // in the order the endgame's MOVE_RECORD_ALL_SMALL generation emits them.
  PATH_MOVE_LISTS_NUM_LANES = 2 * BOARD_DIM,
  // Deepest search path the lists follow: a forced-pass bypass can precede
  // every negamax ply (a bypass never follows another bypass), so at most
  // 2 * plies moves reach a leaf, and a stuck-tile greedy playout then pushes
  // up to MAX_SEARCH_DEPTH + 1 - plies more (see
  // negamax_greedy_leaf_playout). The total is at most
  // 2 * MAX_SEARCH_DEPTH + 1; the rest is slack.
  PATH_MOVE_LISTS_MAX_PATH = 2 * MAX_SEARCH_DEPTH + 4,
  // Per-tile-type counts in 4-bit fields (the blank at index 0), packed into
  // 64-bit words.
  PATH_MOVE_LISTS_COUNT_WORDS = (MAX_ALPHABET_SIZE + 15) / 16,
};

// Per-tile-type counts of a rack or of a move's placed tiles. Neither exceeds
// RACK_SIZE, so every field's high bit is free to act as the borrow sentinel
// of the O(1) sub-multiset test in path_move_lists.c.
typedef struct PackedCounts {
  uint64_t words[PATH_MOVE_LISTS_COUNT_WORDS];
} PackedCounts;

// One materialized list: plays partitioned by lane (lane L spans
// [lane_begin[L], lane_begin[L + 1])), each play's placed-tile multiset packed
// alongside for the rack filter. The trailing pass of a generation is not
// stored; the consumer appends it.
typedef struct PathListSlot {
  SmallMove *moves;
  PackedCounts *used;
  int count;
  int capacity;
  int lane_begin[PATH_MOVE_LISTS_NUM_LANES + 1];
} PathListSlot;

// The legal-play lists of both sides of an endgame search, maintained
// incrementally along the search path instead of regenerated from scratch at
// every node. A node's list is derived from the same side's list two moves
// up the path:
//
//   - lanes touched by neither intervening move (see the lane-influence
//     derivation in path_move_lists.c) carry over verbatim, filtered by a
//     rack sub-multiset test when the side's own intervening move consumed
//     tiles;
//   - in touched lanes, only the anchors whose plays can reach a square
//     either move changed (see move_lane_hits in path_move_lists.c) are
//     regenerated on the current board (generate_small_moves_in_lanes),
//     which also produces every newly enabled play; the other anchors' plays
//     carry over, filtered the same way. A play spanning no changed square
//     is unaffected, and is generated from the leftmost anchor in its span
//     on both boards, so the two sources are disjoint by construction and
//     merge into generation order by anchor, with no deduplication needed.
//
// The result is identical to a scratch generation, in the same order, so a
// consumer cannot tell the difference. The caller owns the discipline the
// derivation chain relies on: seed both sides' root lists, push every move
// made along the path (and pop it on unmake), and request moves_at only once
// every shallower position of the current path has been materialized the
// same way -- the natural shape of a depth-first search, where each node
// generates before descending. Slots are indexed by path length and
// overwritten freely as sibling subtrees revisit a depth.
// One lane's best plays for the side to move at the leaves under one parent
// (see path_move_lists_best_play): the best that uses the whole rack and the
// best that does not, kept apart because the go-out bonus a whole-rack play
// earns depends on which sibling's opponent rack it ends.
typedef struct LaneBestPlays {
  SmallMove whole_rack;
  SmallMove partial;
  bool has_whole_rack;
  bool has_partial;
} LaneBestPlays;

typedef struct PathMoveLists {
  PathListSlot roots[2];
  PathListSlot slots[PATH_MOVE_LISTS_MAX_PATH];
  // Lane-influence mask and PLAY-ness of the move made at each path index.
  uint64_t masks[PATH_MOVE_LISTS_MAX_PATH];
  bool played[PATH_MOVE_LISTS_MAX_PATH];
  // Per lane, the squares the move made at each path index changed for the
  // plays of that lane (bit c: column c of a row, row c of a column): a play
  // spanning none of them is unaffected by the move.
  uint32_t hits[PATH_MOVE_LISTS_MAX_PATH][PATH_MOVE_LISTS_NUM_LANES];
  int length;
  // Scratch for the lane regeneration: list count after each lane, the
  // squares changed since the parent list, the lane's anchors, and the
  // anchors regenerated.
  int lane_end[PATH_MOVE_LISTS_NUM_LANES];
  uint32_t lane_hits[PATH_MOVE_LISTS_NUM_LANES];
  uint32_t lane_anchors[PATH_MOVE_LISTS_NUM_LANES];
  uint32_t lane_generated[PATH_MOVE_LISTS_NUM_LANES];
  // A fresh stamp for every push (and root seeding), so the path prefix that
  // ends at a move is identified by that move's stamp: a prefix revisited
  // through a new push gets a new one.
  uint64_t stamps[PATH_MOVE_LISTS_MAX_PATH];
  uint64_t root_stamp;
  uint64_t next_stamp;
  // The stamp of the move that reached each slot's position when moves_at
  // last built that slot (see path_move_lists_slot_is_current).
  uint64_t slot_stamps[PATH_MOVE_LISTS_MAX_PATH];
  // Per-lane best plays shared by sibling positions, one cache per path
  // length (so a node and its children keep theirs): at length L they are
  // valid below the prefix stamped sibling_stamp[L], for the lanes in
  // sibling_valid[L].
  LaneBestPlays sibling_lanes[PATH_MOVE_LISTS_MAX_PATH]
                             [PATH_MOVE_LISTS_NUM_LANES];
  uint64_t sibling_valid[PATH_MOVE_LISTS_MAX_PATH];
  uint64_t sibling_stamp[PATH_MOVE_LISTS_MAX_PATH];
} PathMoveLists;

PathMoveLists *path_move_lists_create(void);
void path_move_lists_destroy(PathMoveLists *lists);

// Forget the path; the root lists stay until re-seeded.
void path_move_lists_reset(PathMoveLists *lists);

// Seed side `side`'s list on the root board (side 0 moves first at the root)
// from `move_list`, a MOVE_RECORD_ALL_SMALL generation ending in its pass.
void path_move_lists_set_root(PathMoveLists *lists, int side,
                              const MoveList *move_list);

// Record `move` as the next move along the path. `board` must still be in
// its pre-move state.
void path_move_lists_push(PathMoveLists *lists, const Board *board,
                          const Move *move);
void path_move_lists_pop(PathMoveLists *lists);

// The side-to-move's plays at the current path position (length >= 1),
// excluding the pass. `args` describes the MOVE_RECORD_ALL_SMALL generation a
// scratch path would run here; its game supplies the board and rack and its
// move_list is scratch space. The returned pointer is valid until the next
// call at the same or a shallower path length.
int path_move_lists_moves_at(PathMoveLists *lists, const MoveGenArgs *args,
                             const SmallMove **out_moves);

// Stores `move_list`, a MOVE_RECORD_ALL_SMALL generation at the current path
// position (length >= 1) ending in its pass, as that position's list: what
// path_move_lists_moves_at would build there, so later positions can derive
// from it.
void path_move_lists_set_current(PathMoveLists *lists,
                                 const MoveList *move_list);

// Whether the slot of path length `length` (>= 1) holds the list of the
// current path's position there: moves_at has built it since the move that
// reached that position was pushed. A node that searched its children without
// building its own list (best_play only) leaves it stale.
static inline bool path_move_lists_slot_is_current(const PathMoveLists *lists,
                                                   int length) {
  return lists->slot_stamps[length] == lists->stamps[length - 1];
}

// The lanes path_move_lists_moves_at / path_move_lists_best_play would
// regenerate at the current path position (length >= 1): the union of the last
// two pushed moves' lane-influence masks. Zero means nothing is regenerated.
static inline uint64_t
path_move_lists_pending_lanes(const PathMoveLists *lists) {
  uint64_t pending = lists->masks[lists->length - 1];
  if (lists->length >= 2) {
    pending |= lists->masks[lists->length - 2];
  }
  return pending;
}

// The single best play of the list moves_at would return, found without
// materializing that list. Sibling leaves (the same path but for the last
// move) share every lane that last move leaves alone, along with the rack of
// the side to move, so those lanes' best plays are found once per parent and
// kept in sibling_lanes: their plays are scanned in place in the side's list
// two moves up (through the rack sub-multiset filter when the side's own
// previous move placed tiles), except in the lanes that move touched, whose
// anchors that can reach a square it changed are regenerated instead, as in
// moves_at. Their best play is the incumbent that a MOVE_RECORD_BEST_SMALL
// generation over the last move's lanes is shadow-pruned against
// (generate_best_small_move_in_lanes), so most of those lanes' plays are never
// enumerated. A play ranks by score plus the go-out bonus when it uses the
// whole rack, ties going to the larger tiny_move, as in MOVE_RECORD_BEST_SMALL.
// `best_small_args` is the MOVE_RECORD_BEST_SMALL generation a scratch path
// would run here (its best_small_out_bonus is the go-out bonus; its move_list
// is scratch space, large enough for every play of the regenerated lanes).
// Returns false iff the side has no play, where the caller plays the pass.
// Nothing is stored in the slot of the current length, so a later call at
// length + 2 must not rely on it (use moves_at where that happens); every
// shallower slot is read, under the same discipline as moves_at.
bool path_move_lists_best_play(PathMoveLists *lists,
                               const MoveGenArgs *best_small_args,
                               SmallMove *out_best);

#endif
