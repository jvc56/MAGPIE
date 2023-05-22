#ifndef ANCHOR_H
#define ANCHOR_H

#include <stdint.h>

typedef struct Anchor {
    int row;
    int col;
    int last_anchor_col;
    int transpose_state;
    int vertical;
    float highest_possible_equity;
} Anchor;

typedef struct AnchorList {
    int count;
    Anchor ** anchors;
} AnchorList;

AnchorList * create_anchor_list();
void destroy_anchor_list(AnchorList * al);
void insert_anchor(AnchorList* al, int row, int col, int last_anchor_col, int transpose_state, int vertical, float highest_possible_equity);
void reset_anchor_list(AnchorList * al);

#endif