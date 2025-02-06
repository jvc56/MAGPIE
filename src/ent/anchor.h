#ifndef ANCHOR_H
#define ANCHOR_H

#include <stdlib.h>

#include "../def/board_defs.h"

#include "../ent/equity.h"

#include "../util/string_util.h"
#include "../util/util.h"

typedef struct Anchor {
  // The highest possibly equity
  // that can be achieved from this
  // anchor column.
  Equity highest_possible_equity;
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
} Anchor;

typedef struct AnchorList {
  int count;
  Anchor **anchors;
} AnchorList;

static inline AnchorList *anchor_list_create(void) {
  AnchorList *al = malloc_or_die(sizeof(AnchorList));
  al->count = 0;
  al->anchors = malloc_or_die((sizeof(Anchor *)) * ((BOARD_DIM) * (BOARD_DIM)));
  for (int i = 0; i < ((BOARD_DIM) * (BOARD_DIM)); i++) {
    al->anchors[i] = malloc_or_die(sizeof(Anchor));
  }
  return al;
}

typedef struct AnchorHeap {
  int count;
  Anchor anchors[ANCHOR_HEAP_CAPACITY];
} AnchorHeap;

static inline void anchor_destroy(Anchor *anchor) {
  if (!anchor) {
    return;
  }
  free(anchor);
}

static inline void anchor_list_destroy(AnchorList *al) {
  if (!al) {
    return;
  }
  for (int i = 0; i < ((BOARD_DIM) * (BOARD_DIM)); i++) {
    anchor_destroy(al->anchors[i]);
  }
  free(al->anchors);
  free(al);
}

static inline int anchor_get_col(const AnchorList *al, int index) {
  return al->anchors[index]->col;
}

static inline int anchor_get_dir(const AnchorList *al, int index) {
  return al->anchors[index]->dir;
}

static inline Equity anchor_get_highest_possible_equity(const AnchorList *al,
                                                        int index) {
  return al->anchors[index]->highest_possible_equity;
}

static inline int anchor_get_last_anchor_col(const AnchorList *al, int index) {
  return al->anchors[index]->last_anchor_col;
}

static inline int anchor_get_row(const AnchorList *al, int index) {
  return al->anchors[index]->row;
}

static inline int anchor_list_get_count(const AnchorList *al) {
  return al->count;
}

static inline void anchor_list_add_anchor(AnchorList *al, int row, int col,
                                          int last_anchor_col, int dir,
                                          Equity highest_possible_equity) {
  int i = al->count;
  al->anchors[i]->row = row;
  al->anchors[i]->col = col;
  al->anchors[i]->last_anchor_col = last_anchor_col;
  al->anchors[i]->dir = dir;
  al->anchors[i]->highest_possible_equity = highest_possible_equity;
  al->count++;
}

static inline void
anchor_heap_add_unheaped_anchor(AnchorHeap *ah, uint8_t row, uint8_t col,
                                uint8_t last_anchor_col, uint8_t dir,
                                Equity highest_possible_equity) {
  const int i = ah->count;
  ah->anchors[i].row = row;
  ah->anchors[i].col = col;
  ah->anchors[i].last_anchor_col = last_anchor_col;
  ah->anchors[i].dir = dir;
  ah->anchors[i].highest_possible_equity = highest_possible_equity;
  ah->count++;
}

static inline void anchor_list_reset(AnchorList *al) { al->count = 0; }

static inline void anchor_heap_reset(AnchorHeap *ah) { ah->count = 0; }

static inline bool anchor_is_better(const Anchor *anchor_a,
                                    const Anchor *anchor_b) {
  return anchor_a->highest_possible_equity > anchor_b->highest_possible_equity;
}

static inline void swap_anchors(Anchor *a, Anchor *b) {
  Anchor temp;
  memory_copy(&temp, a, sizeof(Anchor));
  memory_copy(a, b, sizeof(Anchor));
  memory_copy(b, &temp, sizeof(Anchor));
}

static inline int anchor_compare(const void *a, const void *b) {
  const Anchor *anchor_a = *(const Anchor **)a;
  const Anchor *anchor_b = *(const Anchor **)b;
  if (anchor_a->highest_possible_equity > anchor_b->highest_possible_equity) {
    return -1;
  } else if (anchor_a->highest_possible_equity <
             anchor_b->highest_possible_equity) {
    return 1;
  } else {
    return 0;
  }
}

static inline void anchor_list_sort(AnchorList *al) {
  qsort(al->anchors, al->count, sizeof(Anchor *), anchor_compare);
}

static inline void anchor_heapify_up(AnchorHeap *heap, int index) {
  int parent_node = (index - 1) / 2;

  if (index > 0 &&
      anchor_is_better(&heap->anchors[index], &heap->anchors[parent_node])) {
    swap_anchors(&heap->anchors[parent_node], &heap->anchors[index]);
    anchor_heapify_up(heap, parent_node);
  }
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

static inline void anchor_heap_insert(AnchorHeap *heap, int row, int col,
                                      int last_anchor_col, int dir,
                                      double highest_possible_equity) {
  Anchor *new_anchor = &heap->anchors[heap->count];
  new_anchor->row = row;
  new_anchor->col = col;
  new_anchor->last_anchor_col = last_anchor_col;
  new_anchor->dir = dir;
  new_anchor->highest_possible_equity = highest_possible_equity;

  anchor_heapify_up(heap, heap->count);
  heap->count++;
}

// O(n)
static inline void anchor_heap_heapify_all(AnchorHeap *heap) {
  for (int node_idx = heap->count / 2; node_idx >= 0; node_idx--) {
    anchor_heapify_down(heap, node_idx);
  }
}

static inline Anchor anchor_heap_extract_max(AnchorHeap *heap) {
  const Anchor max_anchor = heap->anchors[0];
  heap->anchors[0] = heap->anchors[heap->count - 1];
  anchor_heapify_down(heap, 0);
  heap->count--;
  return max_anchor;
}

#endif