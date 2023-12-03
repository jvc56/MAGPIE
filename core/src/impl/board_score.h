#ifndef BOARD_SCORE_H
#define BOARD_SCORE_H

#include "../ent/board.h"
#include "../ent/letter_distribution.h"

int traverse_backwards_for_score(const Board *board,
                                 const LetterDistribution *letter_distribution,
                                 int row, int col);

#endif