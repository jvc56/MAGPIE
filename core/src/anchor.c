#include <stdlib.h>

#include "anchor.h"
#include "board.h"
#include "constants.h"
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

void insert_anchor(AnchorList *al, int row, int col, int last_anchor_col,
                   int transpose_state, int vertical,
                   double highest_possible_equity) {
  int i = al->count;
  for (; i > 0 &&
         al->anchors[i - 1]->highest_possible_equity < highest_possible_equity;
       i--) {
    al->anchors[i]->row = al->anchors[i - 1]->row;
    al->anchors[i]->col = al->anchors[i - 1]->col;
    al->anchors[i]->last_anchor_col = al->anchors[i - 1]->last_anchor_col;
    al->anchors[i]->transpose_state = al->anchors[i - 1]->transpose_state;
    al->anchors[i]->vertical = al->anchors[i - 1]->vertical;
    al->anchors[i]->highest_possible_equity =
        al->anchors[i - 1]->highest_possible_equity;
  }
  al->anchors[i]->row = row;
  al->anchors[i]->col = col;
  al->anchors[i]->last_anchor_col = last_anchor_col;
  al->anchors[i]->transpose_state = transpose_state;
  al->anchors[i]->vertical = vertical;
  al->anchors[i]->highest_possible_equity = highest_possible_equity;
  al->count++;
}

void reset_anchor_list(AnchorList *al) { al->count = 0; }
