#ifndef ANCHOR_H
#define ANCHOR_H

#include "../def/board_defs.h"
#include "../ent/equity.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct Anchor {
  // The highest possible equity that can be achieved from this anchor column
  Equity highest_possible_equity;
  // The highest possible score that can be achieved from this anchor column
  Equity highest_possible_score;
  // The row of the board for this anchor
  unsigned row : 6;
  // The column of the board for this anchor
  unsigned col : 6;

  // The the previous anchor column of the
  // move generator for the row.
  // TODO(olaugh): Replace use of this with leftmost_start_col
  unsigned last_anchor_col : 6;

  // Only used for WMP move generation
  unsigned tiles_to_play : 6;
  unsigned playthrough_blocks : 6;
  unsigned word_length : 6;
  unsigned leftmost_start_col : 6;
  unsigned rightmost_start_col : 6;

  // The direction of the board for
  // this anchor column.
  unsigned dir : 1;
} Anchor;

// Each anchor of the heap has up to ANCHOR_HEAP_ARITY children. A node's
// children are compared all at once, so up to ANCHOR_HEAP_ARITY - 1 slots
// past the last anchor are read: they hold sentinels, which no anchor's bound
// is below (see anchor_heap_set_sentinels). The heap holds far fewer anchors
// than ANCHOR_HEAP_CAPACITY, so the sentinels fit in it; growing it instead
// would move the move generator's fields that follow it.
enum { ANCHOR_HEAP_ARITY = 4 };

typedef struct AnchorHeap {
  int count;
  Anchor anchors[ANCHOR_HEAP_CAPACITY];
} AnchorHeap;

// Appends anchors to the end of the list without any comparisons.
static inline void
anchor_heap_add_unheaped_anchor(AnchorHeap *ah, uint8_t row, uint8_t col,
                                uint8_t last_anchor_col, uint8_t dir,
                                Equity highest_possible_equity,
                                Equity highest_possible_score) {
  const int i = ah->count;
  ah->anchors[i].row = row;
  ah->anchors[i].col = col;
  ah->anchors[i].last_anchor_col = last_anchor_col;
  ah->anchors[i].dir = dir;
  ah->anchors[i].highest_possible_equity = highest_possible_equity;
  ah->anchors[i].highest_possible_score = highest_possible_score;
  ah->count++;
}

// Appends anchors to the end of the list without any comparisons.
static inline void anchor_heap_add_unheaped_wmp_anchor(
    AnchorHeap *ah, uint8_t row, uint8_t col, uint8_t last_anchor_col,
    uint8_t leftmost_start_col, uint8_t rightmost_start_col, uint8_t dir,
    Equity highest_possible_equity, Equity highest_possible_score,
    int tiles_to_play, int playthrough_blocks, int word_length) {
  const int i = ah->count;
  ah->anchors[i].row = row;
  ah->anchors[i].col = col;
  ah->anchors[i].last_anchor_col = last_anchor_col;
  ah->anchors[i].leftmost_start_col = leftmost_start_col;
  ah->anchors[i].rightmost_start_col = rightmost_start_col;
  ah->anchors[i].dir = dir;
  ah->anchors[i].highest_possible_equity = highest_possible_equity;
  ah->anchors[i].highest_possible_score = highest_possible_score;
  ah->anchors[i].tiles_to_play = tiles_to_play;
  ah->anchors[i].playthrough_blocks = playthrough_blocks;
  ah->anchors[i].word_length = word_length;
  ah->count++;
}

static inline void anchor_heap_reset(AnchorHeap *ah) { ah->count = 0; }

// The bound of the sentinels past the last anchor: below every anchor's, so
// a sentinel is never a better child than an anchor.
#define ANCHOR_HEAP_SENTINEL_EQUITY INT32_MIN

// Writes the sentinels into the slots past the last anchor that a sift reads.
static inline void anchor_heap_set_sentinels(AnchorHeap *heap) {
  assert(heap->count + ANCHOR_HEAP_ARITY - 1 <= ANCHOR_HEAP_CAPACITY);
  for (int slot = heap->count; slot < heap->count + ANCHOR_HEAP_ARITY - 1;
       slot++) {
    heap->anchors[slot].highest_possible_equity = ANCHOR_HEAP_SENTINEL_EQUITY;
  }
}

// Sifts the anchor at parent_node down to its place. The heap is 4-ary, half
// as deep as a binary heap, and a node's best child is chosen by comparing
// the four bounds pairwise with no branch on the results. The anchors the
// sifted one passes move up one level each into the hole it leaves, and it is
// written once, at the end (J. W. J. Williams, "Algorithm 232: Heapsort",
// CACM 7, 1964). Anchors with equal bounds may come out in any order: the
// plays they record are ranked by the move comparison's tiebreaks, not by
// when they are found.
static inline void anchor_heapify_down(AnchorHeap *heap, int parent_node) {
  const Anchor sifted = heap->anchors[parent_node];
  int hole = parent_node;
  while (true) {
    const int first_child = hole * ANCHOR_HEAP_ARITY + 1;
    if (first_child >= heap->count) {
      break;
    }
    const Anchor *children = &heap->anchors[first_child];
    const Equity bound0 = children[0].highest_possible_equity;
    const Equity bound1 = children[1].highest_possible_equity;
    const Equity bound2 = children[2].highest_possible_equity;
    const Equity bound3 = children[3].highest_possible_equity;
    const int pick01 = bound1 > bound0;
    const Equity best01 = pick01 ? bound1 : bound0;
    const int pick23 = bound3 > bound2;
    const Equity best23 = pick23 ? bound3 : bound2;
    const int pick_high = best23 > best01;
    const Equity best = pick_high ? best23 : best01;
    if (best <= sifted.highest_possible_equity) {
      break;
    }
    const int child = first_child + (pick_high ? 2 + pick23 : pick01);
    heap->anchors[hole] = heap->anchors[child];
    hole = child;
  }
  heap->anchors[hole] = sifted;
}

// O(n)
static inline void anchor_heapify_all(AnchorHeap *heap) {
  anchor_heap_set_sentinels(heap);
  for (int node_idx = (heap->count - 2) / ANCHOR_HEAP_ARITY; node_idx >= 0;
       node_idx--) {
    anchor_heapify_down(heap, node_idx);
  }
}

// Removes max anchor, returns a copy. The last anchor's slot becomes a
// sentinel before the sift.
static inline Anchor anchor_heap_extract_max(AnchorHeap *heap) {
  const Anchor max_anchor = heap->anchors[0];
  heap->count--;
  heap->anchors[0] = heap->anchors[heap->count];
  heap->anchors[heap->count].highest_possible_equity =
      ANCHOR_HEAP_SENTINEL_EQUITY;
  anchor_heapify_down(heap, 0);
  return max_anchor;
}

#endif
