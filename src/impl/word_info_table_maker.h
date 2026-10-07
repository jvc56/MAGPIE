#ifndef WORD_INFO_TABLE_MAKER_H
#define WORD_INFO_TABLE_MAKER_H

#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"
#include "../ent/word_info_table.h"

WordInfoTable *make_word_info_table_from_words(const DictionaryWordList *words);
// num_threads builds the position-length rows (see
// word_info_table_build_position_lengths).
WordInfoTable *make_word_info_table_from_kwg(const KWG *kwg, int num_threads);

#endif
