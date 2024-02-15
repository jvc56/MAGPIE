#ifndef WORD_PRUNE_H
#define WORD_PRUNE_H

#include "../ent/dictionary_word.h"
#include "../ent/game.h"
#include "../ent/kwg.h"

typedef struct BoardRow {
  int num_cols;
  uint8_t *letters;
} BoardRow;

typedef struct BoardRows {
  BoardRow *rows;
  int num_rows;
} BoardRows;

void generate_possible_words(Game *game, const KWG *override_kwg,
                             DictionaryWordList *possible_word_list);

#endif // WORD_PRUNE_H