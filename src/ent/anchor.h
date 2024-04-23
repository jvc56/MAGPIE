#ifndef ANCHOR_H
#define ANCHOR_H

#include <stdbool.h>

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
  int nearest_left_playthrough_tile_col;
  int nearest_right_playthrough_tile_col;
  // The highest possibly equity
  // that can be achieved from this
  // anchor column.
  double highest_possible_equity;
} Anchor;

typedef struct AnchorList {
  int count;
  Anchor **anchors;
} AnchorList;

AnchorList *anchor_list_create();
void anchor_list_destroy(AnchorList *al);

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

static inline int
anchor_get_nearest_left_playthrough_tile_col(const AnchorList *al, int index) {
  return al->anchors[index]->nearest_left_playthrough_tile_col;
}

static inline int
anchor_get_nearest_right_playthrough_tile_col(const AnchorList *al, int index) {
  return al->anchors[index]->nearest_right_playthrough_tile_col;
}

static inline int anchor_list_get_count(const AnchorList *al) {
  return al->count;
}

static inline void
anchor_list_add_anchor(AnchorList *al, int row, int col, int last_anchor_col,
                       int dir, int nearest_left_playthrough_tile_col,
                       int nearest_right_playthrough_tile_col,
                       double highest_possible_equity) {
  int i = al->count;
  al->anchors[i]->row = row;
  al->anchors[i]->col = col;
  al->anchors[i]->last_anchor_col = last_anchor_col;
  al->anchors[i]->dir = dir;
  al->anchors[i]->nearest_left_playthrough_tile_col =
      nearest_left_playthrough_tile_col;
  al->anchors[i]->nearest_right_playthrough_tile_col =
      nearest_right_playthrough_tile_col;
  al->anchors[i]->highest_possible_equity = highest_possible_equity;
  al->count++;
}

static inline void anchor_list_reset(AnchorList *al) { al->count = 0; }

void anchor_list_sort(AnchorList *al);

#endif