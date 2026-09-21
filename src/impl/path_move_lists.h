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
  // every negamax ply (a bypass never follows another bypass), plus slack.
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
//   - touched lanes are dropped wholesale and regenerated on the current
//     board (generate_small_moves_in_lanes), which also produces every newly
//     enabled play -- the two sources are disjoint by construction, so no
//     deduplication is needed.
//
// The result is identical to a scratch generation, in the same order, so a
// consumer cannot tell the difference. The caller owns the discipline the
// derivation chain relies on: seed both sides' root lists, push every move
// made along the path (and pop it on unmake), and request moves_at only once
// every shallower position of the current path has been materialized the
// same way -- the natural shape of a depth-first search, where each node
// generates before descending. Slots are indexed by path length and
// overwritten freely as sibling subtrees revisit a depth.
typedef struct PathMoveLists {
  PathListSlot roots[2];
  PathListSlot slots[PATH_MOVE_LISTS_MAX_PATH];
  // Lane-influence mask and PLAY-ness of the move made at each path index.
  uint64_t masks[PATH_MOVE_LISTS_MAX_PATH];
  bool played[PATH_MOVE_LISTS_MAX_PATH];
  int length;
  // Scratch for the lane regeneration: list count after each lane.
  int lane_end[PATH_MOVE_LISTS_NUM_LANES];
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

#endif
