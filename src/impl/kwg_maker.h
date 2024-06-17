#ifndef KWG_MAKER_H
#define KWG_MAKER_H

#include "../def/kwg_defs.h"
#include "../ent/conversion_results.h"
#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"

typedef struct ConversionArgs {
  const char *conversion_type_string;
  const char *input_filename;
  const char *output_filename;
  const LetterDistribution *ld;
} ConversionArgs;

KWG *make_kwg_from_words(const DictionaryWordList *words,
                         kwg_maker_output_t output, kwg_maker_merge_t merge);

void kwg_write_words(const KWG *kwg, uint32_t node_index,
                     DictionaryWordList *words, bool *nodes_reached);

void add_gaddag_strings(const DictionaryWordList *words,
                        DictionaryWordList *gaadag_strings);

conversion_status_t convert(ConversionArgs *args,
                            ConversionResults *conversion_results);
#endif