#ifndef WORDS_H
#define WORDS_H

#include <stdint.h>

#include "board.h"
#include "constants.h"
#include "kwg.h"

typedef struct FormedWord {
  uint8_t word[BOARD_DIM];
  int word_length;
  int valid;
} FormedWord;

typedef struct FormedWords {
  int num_words;
  FormedWord words[RACK_SIZE + 1]; // max number of words we can form
} FormedWords;

// populate the validity of the formed words passed in.
void populate_word_validities(FormedWords *ws, KWG *kwg);
FormedWords *words_played(Board *board, uint8_t word[], int word_start_index,
                          int word_end_index, int row, int col, int vertical);
#endif