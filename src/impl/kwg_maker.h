
#ifndef KWG_MAKER_H
#define KWG_MAKER_H

#include "../def/kwg_defs.h"
#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"

KWG *make_kwg_from_words(const DictionaryWordList *words,
                         kwg_maker_output_t output, kwg_maker_merge_t merge);

// Optimized version for small dictionaries (endgame wordprune case).
// Uses appropriately sized data structures based on word count.
KWG *make_kwg_from_words_small(const DictionaryWordList *words,
                               kwg_maker_output_t output,
                               kwg_maker_merge_t merge);

void kwg_write_words(const KWG *kwg, uint32_t node_index,
                     DictionaryWordList *words, bool *nodes_reached);

void kwg_write_gaddag_strings(const KWG *kwg, uint32_t node_index,
                              DictionaryWordList *words, bool *nodes_reached);

void add_gaddag_strings(const DictionaryWordList *words,
                        DictionaryWordList *gaddag_strings);

#endif
