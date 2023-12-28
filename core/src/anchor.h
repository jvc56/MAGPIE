#ifndef ANCHOR_H
#define ANCHOR_H

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "constants.h"
#include "rack.h"

typedef struct ShadowLimit {
  int num_playthrough;
  double highest_equity;
} ShadowLimit;

typedef struct Anchor {
  int row;
  int col;
  int last_anchor_col;
  bool transposed;
  int dir;
  int min_num_playthrough;
  int max_num_playthrough;
  int min_tiles_to_play;
  int max_tiles_to_play;
  int max_tiles_starting_left_by[(BOARD_DIM)];
  double highest_possible_equity;
  double highest_equity_by_length[(RACK_SIZE + 1)];
  ShadowLimit shadow_limit_table[(BOARD_DIM)][(RACK_SIZE + 1)];
} Anchor;

typedef struct AnchorList {
  int count;
  Anchor **anchors;
} AnchorList;

AnchorList *create_anchor_list();
void destroy_anchor_list(AnchorList *al);
void add_anchor(AnchorList *al, int row, int col, int last_anchor_col,
                bool transposed, int dir, int min_num_playthrough,
                int max_num_playthrough, int min_tiles_to_play,
                int max_tiles_to_play,
                int max_tiles_starting_left_by[(BOARD_DIM)],
                double highest_possible_equity,
                double *highest_equity_by_length,
                ShadowLimit shadow_limit_table[(BOARD_DIM)][(RACK_SIZE + 1)]);
void sort_anchor_list(AnchorList *al);
void reset_anchor_list(AnchorList *al);

#endif