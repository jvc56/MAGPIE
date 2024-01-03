#ifndef ANCHOR_H
#define ANCHOR_H

#include <stdbool.h>

struct AnchorList;
typedef struct AnchorList AnchorList;

AnchorList *anchor_list_create();
void anchor_list_destroy(AnchorList *al);

int anchor_get_col(const AnchorList *al, int index);
int anchor_get_dir(const AnchorList *al, int index);
double anchor_get_highest_possible_equity(const AnchorList *al, int index);
int anchor_get_last_anchor_col(const AnchorList *al, int index);
int anchor_get_row(const AnchorList *al, int index);
bool anchor_get_transposed(const AnchorList *al, int index);

int anchor_list_get_count(const AnchorList *al);

void anchor_list_add_anchor(AnchorList *al, int row, int col,
                            int last_anchor_col, bool transposed, int dir,
                            double highest_possible_equity);
void anchor_list_reset(AnchorList *al);
void anchor_list_sort(AnchorList *al);

#endif