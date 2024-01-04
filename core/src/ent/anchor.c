#include "anchor.h"

#include "../def/board_defs.h"

#include "../util/util.h"

typedef struct Anchor {
  int row;
  int col;
  int last_anchor_col;
  bool transposed;
  int dir;
  double highest_possible_equity;
} Anchor;

struct AnchorList {
  int count;
  Anchor **anchors;
};

Anchor *anchor_create() { return malloc_or_die(sizeof(Anchor)); }

AnchorList *anchor_list_create() {
  AnchorList *al = malloc_or_die(sizeof(AnchorList));
  al->count = 0;
  al->anchors = malloc_or_die((sizeof(Anchor *)) * ((BOARD_DIM) * (BOARD_DIM)));
  for (int i = 0; i < ((BOARD_DIM) * (BOARD_DIM)); i++) {
    al->anchors[i] = anchor_create();
  }
  return al;
}

void anchor_destroy(Anchor *anchor) {
  if (!anchor) {
    return;
  }
  free(anchor);
}

void anchor_list_destroy(AnchorList *al) {
  if (!al) {
    return;
  }
  for (int i = 0; i < ((BOARD_DIM) * (BOARD_DIM)); i++) {
    anchor_destroy(al->anchors[i]);
  }
  free(al->anchors);
  free(al);
}

int anchor_get_col(const AnchorList *al, int index) {
  return al->anchors[index]->col;
}

int anchor_get_dir(const AnchorList *al, int index) {
  return al->anchors[index]->dir;
}

double anchor_get_highest_possible_equity(const AnchorList *al, int index) {
  return al->anchors[index]->highest_possible_equity;
}

int anchor_get_last_anchor_col(const AnchorList *al, int index) {
  return al->anchors[index]->last_anchor_col;
}

int anchor_get_row(const AnchorList *al, int index) {
  return al->anchors[index]->row;
}

bool anchor_get_transposed(const AnchorList *al, int index) {
  return al->anchors[index]->transposed;
}

int anchor_list_get_count(const AnchorList *al) { return al->count; }

void anchor_list_add_anchor(AnchorList *al, int row, int col,
                            int last_anchor_col, bool transposed, int dir,
                            double highest_possible_equity) {
  int i = al->count;
  al->anchors[i]->row = row;
  al->anchors[i]->col = col;
  al->anchors[i]->last_anchor_col = last_anchor_col;
  al->anchors[i]->transposed = transposed;
  al->anchors[i]->dir = dir;
  al->anchors[i]->highest_possible_equity = highest_possible_equity;
  al->count++;
}

void anchor_list_reset(AnchorList *al) { al->count = 0; }

int anchor_compare(const void *a, const void *b) {
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

void anchor_list_sort(AnchorList *al) {
  qsort(al->anchors, al->count, sizeof(Anchor *), anchor_compare);
}
