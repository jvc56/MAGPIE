#ifndef ANCHOR_H
#define ANCHOR_H

#include <_types/_uint16_t.h>
#include <_types/_uint8_t.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/rack_defs.h"

#include "../util/util.h"

#define ANCHOR_LIST_CAPACITY BOARD_DIM * BOARD_DIM * (RACK_SIZE + 1)

typedef struct Anchor {
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

  // If tiles_to_play < MIN_TILES_FOR_WMP_GEN, these are maximums, with the
  // respective minimums being 1 tile played and 0 playthrough blocks.
  // If tiles to play >= MIN_TILES_FOR_WMP_GEN, only these exact numbers are
  // permitted.
  uint8_t tiles_to_play;
  uint8_t playthrough_blocks;

  uint16_t highest_possible_score;

  // The highest possibly equity
  // that can be achieved from this
  // anchor column.
  double highest_possible_equity;
} Anchor;

typedef struct AnchorList {
  int count;
  Anchor **anchors;
} AnchorList;

static inline AnchorList *anchor_list_create(void) {
  AnchorList *al = (AnchorList *)malloc_or_die(sizeof(AnchorList));
  al->count = 0;
  al->anchors = (Anchor **)malloc_or_die((sizeof(Anchor *)) * ANCHOR_LIST_CAPACITY);
  for (int i = 0; i < ANCHOR_LIST_CAPACITY; i++) {
    al->anchors[i] = (Anchor *)malloc_or_die(sizeof(Anchor));
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
  for (int i = 0; i < ANCHOR_LIST_CAPACITY; i++) {
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
                                          uint16_t highest_possible_score,
                                          double highest_possible_equity) {
  int i = al->count;
  al->anchors[i]->row = row;
  al->anchors[i]->col = col;
  al->anchors[i]->last_anchor_col = last_anchor_col;
  al->anchors[i]->dir = dir;
  al->anchors[i]->highest_possible_score = highest_possible_score;
  al->anchors[i]->highest_possible_equity = highest_possible_equity;

  // Hackily setting these to maximums just to test recursive_gen respecting
  // the values.
  al->anchors[i]->tiles_to_play = 7;
  al->anchors[i]->playthrough_blocks = 8; // FIXME
  al->count++;
}

static inline void anchor_list_add_anchor_copy(AnchorList *al, const Anchor *anchor) {
  int i = al->count;
  al->anchors[i]->row = anchor->row;
  al->anchors[i]->col = anchor->col;
  al->anchors[i]->last_anchor_col = anchor->last_anchor_col;
  al->anchors[i]->dir = anchor->dir;
  al->anchors[i]->highest_possible_score = anchor->highest_possible_score;
  al->anchors[i]->highest_possible_equity = anchor->highest_possible_equity;
  al->anchors[i]->tiles_to_play = anchor->tiles_to_play;
  al->anchors[i]->playthrough_blocks = anchor->playthrough_blocks;
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