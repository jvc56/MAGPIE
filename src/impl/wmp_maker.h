#ifndef WMP_MAKER_H
#define WMP_MAKER_H

#include "../ent/dictionary_word.h"
#include "../ent/wmp.h"

WMP *make_wmp_from_words(const DictionaryWordList *words,
                         const LetterDistribution *ld);

#endif