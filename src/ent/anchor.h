#ifndef ANCHOR_H
#define ANCHOR_H

#include <stdlib.h>
#include <string.h>

#include "../def/board_defs.h"

#include "../ent/bit_rack.h"
#include "../ent/equity.h"

#include "../util/string_util.h"

typedef struct Anchor {
  // All the tiles that must be played through for this (sub)anchor.
  // Only used by WMP Move Gen.
  BitRack playthrough;
  // Highest possible equity that can be achieved from this anchor.
  Equity highest_possible_equity;
  // Highest possible score for this anchor. Only used by WMP Move Gen.
  Equity highest_possible_score;
  // The row of the board for this anchor
  uint8_t row;
  // The column of the board for this anchor
  uint8_t col;
  // The the previous anchor column of the
  // move generator for the row.
  uint8_t last_anchor_col;
  // The direction of the board for
  // this anchor column.
  uint8_t dir;
  // Number of rack tiles used playing in this anchor.
  // Only used by WMP Move Gen.
  uint8_t tiles_to_play;
  // Number of playthrough blocks used by the anchor. Word will only play
  // rightward/downward from the anchor square.
  // Only used by WMP Move Gen.
  uint8_t playthrough_blocks;
} Anchor;

typedef struct AnchorHeap {
  int count;
  Anchor anchors[ANCHOR_HEAP_CAPACITY];
} AnchorHeap;

// Appends anchors to the end of the list without any comparisons.
static inline void anchor_heap_add_unheaped_anchor(
    AnchorHeap *ah, uint8_t row, uint8_t col, uint8_t last_anchor_col,
    uint8_t dir, Equity highest_possible_equity) {
  const int i = ah->count;
  ah->anchors[i].row = row;
  ah->anchors[i].col = col;
  ah->anchors[i].last_anchor_col = last_anchor_col;
  ah->anchors[i].dir = dir;
  ah->anchors[i].highest_possible_equity = highest_possible_equity;
  ah->count++;
}

static inline void anchor_heap_add_unheaped_wmp_anchor(
    AnchorHeap *ah, uint8_t row, uint8_t col, uint8_t last_anchor_col,
    uint8_t dir, Equity highest_possible_equity, const BitRack *playthrough,
    Equity highest_possible_score) {
  const int i = ah->count;
  ah->anchors[i].row = row;
  ah->anchors[i].col = col;
  ah->anchors[i].last_anchor_col = last_anchor_col;
  ah->anchors[i].dir = dir;
  ah->anchors[i].highest_possible_equity = highest_possible_equity;
  ah->anchors[i].highest_possible_score = highest_possible_score;
  ah->anchors[i].playthrough = *playthrough;
  ah->count++;
}

static inline void anchor_heap_reset(AnchorHeap *ah) { ah->count = 0; }

static inline bool anchor_is_better(const Anchor *anchor_a,
                                    const Anchor *anchor_b) {
  return anchor_a->highest_possible_equity > anchor_b->highest_possible_equity;
}

static inline void swap_anchors(Anchor *a, Anchor *b) {
  Anchor temp;
  memcpy(&temp, a, sizeof(Anchor));
  memcpy(a, b, sizeof(Anchor));
  memcpy(b, &temp, sizeof(Anchor));
}

static inline void anchor_heapify_down(AnchorHeap *heap, int parent_node) {
  int left = parent_node * 2 + 1;
  int right = parent_node * 2 + 2;
  int min;

  if (left >= heap->count || left < 0)
    left = -1;
  if (right >= heap->count || right < 0)
    right = -1;

  if (left != -1 &&
      anchor_is_better(&heap->anchors[left], &heap->anchors[parent_node]))
    min = left;
  else
    min = parent_node;
  if (right != -1 &&
      anchor_is_better(&heap->anchors[right], &heap->anchors[min]))
    min = right;

  if (min != parent_node) {
    swap_anchors(&heap->anchors[min], &heap->anchors[parent_node]);
    anchor_heapify_down(heap, min);
  }
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
