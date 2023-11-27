#ifndef ANCHOR_H
#define ANCHOR_H

#include <stdbool.h>
#include <stdint.h>

struct AnchorList;
typedef struct AnchorList AnchorList;

AnchorList *create_anchor_list();
void destroy_anchor_list(AnchorList *al);
int get_number_of_anchors(const AnchorList *al);
int get_anchor_row(const AnchorList *al, int index);
int get_anchor_col(const AnchorList *al, int index);
int get_anchor_last_anchor_col(const AnchorList *al, int index);
bool get_anchor_transposed(const AnchorList *al, int index);
int get_anchor_dir(const AnchorList *al, int index);
double get_anchor_highest_possible_equity(const AnchorList *al, int index);
void add_anchor(AnchorList *al, int row, int col, int last_anchor_col,
                bool transposed, int dir, double highest_possible_equity);
void sort_anchor_list(AnchorList *al);
void reset_anchor_list(AnchorList *al);

#endif