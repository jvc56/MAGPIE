#ifndef KWG_MAKER_H
#define KWG_MAKER_H

#include "../def/kwg_defs.h"
#include "../ent/config.h"
#include "../ent/conversion_results.h"
#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"

KWG *make_kwg_from_words(const DictionaryWordList *words,
                         kwg_maker_output_t output, kwg_maker_merge_t merge);

void kwg_write_words(const KWG *kwg, int node_index, DictionaryWordList *words,
                     bool *nodes_reached);

void add_gaddag_strings(const DictionaryWordList *words,
                        DictionaryWordList *gaadag_strings);

conversion_status_t convert(const Config *config,
                            ConversionResults *conversion_results);
#endif