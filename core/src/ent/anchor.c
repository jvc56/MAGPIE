#include "../def/board_defs.h"

#include "../util/util.h"

#include "anchor.h"

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

int get_number_of_anchors(const AnchorList *al) { return al->count; }

int get_anchor_row(const AnchorList *al, int index) {
  return al->anchors[index]->row;
}

int get_anchor_col(const AnchorList *al, int index) {
  return al->anchors[index]->col;
}

int get_anchor_last_anchor_col(const AnchorList *al, int index) {
  return al->anchors[index]->last_anchor_col;
}

bool get_anchor_transposed(const AnchorList *al, int index) {
  return al->anchors[index]->transposed;
}

int get_anchor_dir(const AnchorList *al, int index) {
  return al->anchors[index]->dir;
}

double get_anchor_highest_possible_equity(const AnchorList *al, int index) {
  return al->anchors[index]->highest_possible_equity;
}

void add_anchor(AnchorList *al, int row, int col, int last_anchor_col,
                bool transposed, int dir, double highest_possible_equity) {
  int i = al->count;
  al->anchors[i]->row = row;
  al->anchors[i]->col = col;
  al->anchors[i]->last_anchor_col = last_anchor_col;
  al->anchors[i]->transposed = transposed;
  al->anchors[i]->dir = dir;
  al->anchors[i]->highest_possible_equity = highest_possible_equity;
  al->count++;
}

int compare_anchors(const void *a, const void *b) {
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

void sort_anchor_list(AnchorList *al) {
  qsort(al->anchors, al->count, sizeof(Anchor *), compare_anchors);
}

void reset_anchor_list(AnchorList *al) { al->count = 0; }
