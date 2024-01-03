#ifndef WORDS_H
#define WORDS_H

#include <stdint.h>

#include "board.h"
#include "kwg.h"
#include "rack.h"

struct FormedWords;
typedef struct FormedWords FormedWords;

FormedWords *formed_words_create(Board *board, uint8_t word[],
                                 int word_start_index, int word_end_index,
                                 int row, int col, int dir);
void formed_words_destroy(FormedWords *fw);

int formed_words_get_num_words(const FormedWords *fw);
const uint8_t *formed_words_get_word(const FormedWords *fw, int word_index);
int formed_words_get_word_length(const FormedWords *fw, int word_index);
int formed_words_get_word_valid(const FormedWords *fw, int word_index);
int formed_words_get_word_letter(const FormedWords *fw, int word_index,
                                 int letter_index);

// populate the validity of the formed words passed in.
void formed_words_populate_validities(const KWG *kwg, FormedWords *ws);

#endif