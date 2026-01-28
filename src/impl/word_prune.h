#ifndef WORD_PRUNE_H
#define WORD_PRUNE_H

#include "../ent/dictionary_word.h"
#include "../ent/game.h"
#include "../ent/kwg.h"

typedef struct BoardRow {
  MachineLetter letters[BOARD_DIM];
} BoardRow;

typedef struct BoardRows {
  BoardRow rows[BOARD_DIM * 2];
  int num_rows;
} BoardRows;

void generate_possible_words(const Game *game, const KWG *override_kwg,
                             DictionaryWordList *possible_word_list);

// Timing functions (for debugging, will be removed before PR review)
void word_prune_print_timing_stats(void);
void word_prune_reset_timing_stats(void);

#endif // WORD_PRUNE_H