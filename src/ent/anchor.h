#ifndef ANCHOR_H
#define ANCHOR_H

#include <stdlib.h>

#include "../def/board_defs.h"

#include "../util/util.h"

typedef struct Anchor {
  // The row of the board for this anchor
  int row;
  // The column of the board for this anchor
  int col;
  // The the previous anchor column of the
  // move generator for the row.
  int last_anchor_col;
  // The direction of the board for
  // this anchor column.
  int dir;
  // The highest possibly equity
  // that can be achieved from this
  // anchor column.
  double highest_possible_equity;
} Anchor;

typedef struct AnchorList {
  int count;
  Anchor **anchors;
} AnchorList;

static inline AnchorList *anchor_list_create() {
  AnchorList *al = malloc_or_die(sizeof(AnchorList));
  al->count = 0;
  al->anchors = malloc_or_die((sizeof(Anchor *)) * ((BOARD_DIM) * (BOARD_DIM)));
  for (int i = 0; i < ((BOARD_DIM) * (BOARD_DIM)); i++) {
    al->anchors[i] = malloc_or_die(sizeof(Anchor));
  }
  return al;
}

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

static inline double anchor_get_highest_possible_equity(const AnchorList *al,
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
                                          double highest_possible_equity) {
  int i = al->count;
  al->anchors[i]->row = row;
  al->anchors[i]->col = col;
  al->anchors[i]->last_anchor_col = last_anchor_col;
  al->anchors[i]->dir = dir;
  al->anchors[i]->highest_possible_equity = highest_possible_equity;
  al->count++;
}

static inline void anchor_list_reset(AnchorList *al) { al->count = 0; }

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

#endif