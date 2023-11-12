#ifndef ANCHOR_H
#define ANCHOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Anchor {
  int row;
  int col;
  int last_anchor_col;
  bool transposed;
  int dir;
  double highest_possible_equity;
} Anchor;

typedef struct AnchorList {
  int count;
  Anchor **anchors;
} AnchorList;

AnchorList *create_anchor_list();
void destroy_anchor_list(AnchorList *al);
void add_anchor(AnchorList *al, int row, int col, int last_anchor_col,
                bool transposed, int dir, double highest_possible_equity);
void sort_anchor_list(AnchorList *al);
void reset_anchor_list(AnchorList *al);

#endif