#include <stdlib.h>

#include "anchor.h"
#include "constants.h"

AnchorList * create_anchor_list() {
    AnchorList *al = malloc(sizeof(AnchorList));
    al->count = 0;
    int anchor_list_size = ((BOARD_DIM) * (BOARD_DIM));
    al->anchors = malloc((sizeof(Anchor*)) * ((BOARD_DIM) * (BOARD_DIM)));
    for (int i = 0; i < ((BOARD_DIM) * (BOARD_DIM)); i++) {
        al->anchors[i] = malloc(sizeof(Anchor));
    }
    return al;
}

void destroy_anchor(Anchor * anchor) {
    free(anchor);
}

void destroy_anchor_list(AnchorList * al) {
    for (int i = 0; i < ((BOARD_DIM) * (BOARD_DIM)); i++) {
        destroy_anchor(al->anchors[i]);
    }
    free(al->anchors);
    free(al);
}

void insert_anchor(AnchorList* al, int row, int col, int last_anchor_col, int transpose_state, int vertical, int highest_possible_score) {
    int i = al->count;
    for (; i > 0 && al->anchors[i-1]->highest_possible_score < highest_possible_score; i--) {
        al->anchors[i]->row = al->anchors[i-1]->row;
        al->anchors[i]->col = al->anchors[i-1]->col;
        al->anchors[i]->last_anchor_col = al->anchors[i-1]->last_anchor_col;
        al->anchors[i]->transpose_state = al->anchors[i-1]->transpose_state;
        al->anchors[i]->vertical = al->anchors[i-1]->vertical;
        al->anchors[i]->highest_possible_score = al->anchors[i-1]->highest_possible_score;
    }
    al->anchors[i]->row = row;
    al->anchors[i]->col = col;
    al->anchors[i]->last_anchor_col = last_anchor_col;
    al->anchors[i]->transpose_state = transpose_state;
    al->anchors[i]->vertical = vertical;
    al->anchors[i]->highest_possible_score = highest_possible_score;
    al->count++;
}

void reset_anchor_list(AnchorList * al) {
    al->count = 0;
}
