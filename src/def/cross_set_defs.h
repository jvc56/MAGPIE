#ifndef CROSS_SET_DEFS_H
#define CROSS_SET_DEFS_H

#include "letter_distribution_defs.h"

enum {
  WORD_DIRECTION_RIGHT = 1,
  WORD_DIRECTION_LEFT = -1,
  SEPARATION_MACHINE_LETTER = 0,
  TRIVIAL_CROSS_SET = ((uint64_t)1 << MAX_ALPHABET_SIZE) - 1,
};

#endif
