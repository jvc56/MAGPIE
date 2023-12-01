#ifndef CROSS_SET_H
#define CROSS_SET_H

#include "../ent/board.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"

int allowed(uint64_t cross_set, uint8_t letter);
void gen_cross_set(const KWG *kwg,
                   const LetterDistribution *letter_distribution, Board *board,
                   int row, int col, int dir, int cross_set_index);
void generate_all_cross_sets(const KWG *kwg_1, const KWG *kwg_2,
                             const LetterDistribution *letter_distribution,
                             Board *board, bool kwgs_are_distinct);

#endif