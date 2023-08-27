#ifndef CROSS_SET_H
#define CROSS_SET_H

#include "board.h"
#include "kwg.h"

int allowed(uint64_t cross_set, uint8_t letter);
void gen_cross_set(Board *board, int row, int col, int dir, int cross_set_index,
                   KWG *kwg, LetterDistribution *letter_distribution);
void generate_all_cross_sets(Board *board, KWG *kwg_1, KWG *kwg_2,
                             LetterDistribution *letter_distribution,
                             int kwgs_are_distinct);

#endif