#ifndef CROSS_SET_H
#define CROSS_SET_H

#include "board.h"
#include "kwg.h"

#define WORD_DIRECTION_RIGHT 1
#define WORD_DIRECTION_LEFT -1
#define SEPARATION_MACHINE_LETTER 0

int allowed(uint64_t cross_set, uint8_t letter);
void gen_cross_set(Board *board, int row, int col, int dir, int cross_set_index,
                   const KWG *kwg,
                   const LetterDistribution *letter_distribution);
void generate_all_cross_sets(Board *board, const KWG *kwg_1, const KWG *kwg_2,
                             const LetterDistribution *letter_distribution,
                             bool kwgs_are_distinct);

#endif