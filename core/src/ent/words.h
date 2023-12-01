#ifndef WORDS_H
#define WORDS_H

#include <stdint.h>

#include "board.h"
#include "constants.h"
#include "kwg.h"
#include "rack.h"

struct FormedWords;
typedef struct FormedWords FormedWords;

// populate the validity of the formed words passed in.
void populate_word_validities(const KWG *kwg, FormedWords *ws);
FormedWords *words_played(Board *board, uint8_t word[], int word_start_index,
                          int word_end_index, int row, int col, int dir);
#endif