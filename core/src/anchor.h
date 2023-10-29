#ifndef ANCHOR_H
#define ANCHOR_H

#include <stdint.h>

#include "rack.h"

typedef struct Anchor {
  int row;
  int col;
  int last_anchor_col;
  int transpose_state;
  int vertical;
  int max_num_playthrough;
  int min_tiles_to_play;
  int max_tiles_to_play;
  double highest_possible_equity;
  double highest_equity_by_length[(RACK_SIZE + 1)];
} Anchor;

typedef struct AnchorList {
  int count;
  Anchor **anchors;
} AnchorList;

AnchorList *create_anchor_list();
void destroy_anchor_list(AnchorList *al);
void add_anchor(AnchorList *al, int row, int col, int last_anchor_col,
                int transpose_state, int vertical, int max_num_playthrough,
                int min_tiles_to_play, int max_tiles_to_play,
                double highest_possible_equity,
                double *highest_equity_by_length);
void sort_anchor_list(AnchorList *al);
void reset_anchor_list(AnchorList *al);

#endif