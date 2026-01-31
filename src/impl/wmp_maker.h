#ifndef WMP_MAKER_H
#define WMP_MAKER_H

#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"
#include "../ent/wmp.h"

// num_threads: number of threads to use (0 means use all available cores)
WMP *make_wmp_from_words(const DictionaryWordList *words,
                         const LetterDistribution *ld, int num_threads);

// Creates a WMP from a KWG using the DAWG portion only.
// The GADDAG portion of the KWG is not used.
// num_threads: number of threads to use (0 means use all available cores)
WMP *make_wmp_from_kwg(const KWG *kwg, const LetterDistribution *ld,
                       int num_threads);

#endif