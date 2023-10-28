#include <stdlib.h>

#include "anchor.h"
#include "board.h"
#include "constants.h"
#include "stdlib.h"
#include "util.h"

AnchorList *create_anchor_list() {
  AnchorList *al = malloc_or_die(sizeof(AnchorList));
  al->count = 0;
  al->anchors = malloc_or_die((sizeof(Anchor *)) * ((BOARD_DIM) * (BOARD_DIM)));
  for (int i = 0; i < ((BOARD_DIM) * (BOARD_DIM)); i++) {
    al->anchors[i] = malloc_or_die(sizeof(Anchor));
  }
  return al;
}

void destroy_anchor(Anchor *anchor) { free(anchor); }

void destroy_anchor_list(AnchorList *al) {
  for (int i = 0; i < ((BOARD_DIM) * (BOARD_DIM)); i++) {
    destroy_anchor(al->anchors[i]);
  }
  free(al->anchors);
  free(al);
}

void add_anchor(AnchorList *al, int row, int col, int last_anchor_col,
                   int transpose_state, int vertical,
                   double highest_possible_equity) {
  int i = al->count;
  al->anchors[i]->row = row;
  al->anchors[i]->col = col;
  al->anchors[i]->last_anchor_col = last_anchor_col;
  al->anchors[i]->transpose_state = transpose_state;
  al->anchors[i]->vertical = vertical;
  al->anchors[i]->highest_possible_equity = highest_possible_equity;
  al->count++;
}

int compare_anchors(const void *a, const void *b) {
  const Anchor *anchor_a = *(const Anchor **)a;
  const Anchor *anchor_b = *(const Anchor **)b;
  if (anchor_a->highest_possible_equity > anchor_b->highest_possible_equity) {
    return -1;
  } else if (anchor_a->highest_possible_equity < anchor_b->highest_possible_equity) {
    return 1;
  } else {
    return 0;
  }
}

void sort_anchor_list(AnchorList *al) {
  qsort(al->anchors, al->count, sizeof(Anchor *), compare_anchors);
}

void reset_anchor_list(AnchorList *al) { al->count = 0; }
