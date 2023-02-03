#ifndef CROSS_SET_H
#define CROSS_SET_H

#include "alphabet.h"
#include "board.h"
#include "gaddag.h"

int allowed(uint64_t cross_set, uint8_t letter);
void gen_cross_set(Board * board, int row, int col, int dir, Gaddag * gaddag, LetterDistribution * letter_distribution);
void generate_all_cross_sets(Board * board, Gaddag * gaddag, LetterDistribution * letter_distribution);

#endif