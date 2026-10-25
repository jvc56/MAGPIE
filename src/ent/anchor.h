#ifndef ANCHOR_H
#define ANCHOR_H

#include "../def/board_defs.h"
#include "../ent/equity.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
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

static inline bool anchor_is_better(const Anchor *anchor_a,
                                    const Anchor *anchor_b) {
  return anchor_a->highest_possible_equity > anchor_b->highest_possible_equity;
}

// Sifts the anchor at parent_node down to its place. The anchors it passes
// move up one level each into the hole it leaves, and it is written once, at
// the end, instead of being swapped at every level, as in the original
// heapsort (J. W. J. Williams, "Algorithm 232: Heapsort", CACM 7, 1964). A
// child moves up only when strictly better than the sifted anchor, and the
// right child is preferred only when strictly better than the left, so ties
// settle as with swaps.
static inline void anchor_heapify_down(AnchorHeap *heap, int parent_node) {
  const Anchor sifted = heap->anchors[parent_node];
  int hole = parent_node;
  while (true) {
    int child = hole * 2 + 1;
    if (child >= heap->count) {
      break;
    }
    if (child + 1 < heap->count &&
        anchor_is_better(&heap->anchors[child + 1], &heap->anchors[child])) {
      child++;
    }
    if (!anchor_is_better(&heap->anchors[child], &sifted)) {
      break;
    }
    heap->anchors[hole] = heap->anchors[child];
    hole = child;
  }
  heap->anchors[hole] = sifted;
}

// O(n)
static inline void anchor_heapify_all(AnchorHeap *heap) {
  for (int node_idx = heap->count / 2; node_idx >= 0; node_idx--) {
    anchor_heapify_down(heap, node_idx);
  }
}

// Removes max anchor, returns a copy.
static inline Anchor anchor_heap_extract_max(AnchorHeap *heap) {
  const Anchor max_anchor = heap->anchors[0];
  heap->anchors[0] = heap->anchors[heap->count - 1];
  anchor_heapify_down(heap, 0);
  heap->count--;
  return max_anchor;
}

#endif
