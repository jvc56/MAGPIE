#ifndef WMP_MAKER_H
#define WMP_MAKER_H

#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"
#include "../ent/wmp.h"
#include <stdatomic.h>

// Optional builder-only progress. Read concurrently using atomic_load.
// One completed unit is a word-length group, not an estimate of elapsed time.
typedef struct {
  _Atomic int
      stage; // 0 words from KWG, 1 words, 2 blanks, 3 double blanks, 4 indexes
  _Atomic int completed;
  _Atomic int total;
} WMPBuildProgress;

WMP *make_wmp_from_kwg_with_progress(const KWG *kwg,
                                     const LetterDistribution *ld,
                                     int num_threads,
                                     WMPBuildProgress *progress);

// num_threads: number of threads to use (0 means use all available cores)
WMP *make_wmp_from_words(const DictionaryWordList *words,
                         const LetterDistribution *ld, int num_threads);

// Creates a WMP from a KWG using the DAWG portion only.
// The GADDAG portion of the KWG is not used.
// num_threads: number of threads to use (0 means use all available cores)
WMP *make_wmp_from_kwg(const KWG *kwg, const LetterDistribution *ld,
                       int num_threads);

#endif