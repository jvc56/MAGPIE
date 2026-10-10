#ifndef WMP_MAKER_H
#define WMP_MAKER_H

#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"
#include "../ent/wmp.h"
#include <stdatomic.h>
#include <stdbool.h>

// Optional builder-only progress. Use the snapshot helper for coherent reads.
// One completed unit is a word-length group, not an estimate of elapsed time.
typedef struct {
  _Atomic unsigned int sequence;
  _Atomic int
      stage; // 0 words from KWG, 1 words, 2 blanks, 3 double blanks, 4 indexes
  _Atomic int completed;
  _Atomic int total;
  _Atomic bool
      cancelled; // caller may request cancellation between length groups
} WMPBuildProgress;

// Stage transitions happen only after the previous stage's workers join.
static inline void wmp_build_progress_set_stage(WMPBuildProgress *progress,
                                                int stage, int total) {
  if (!progress) {
    return;
  }
  atomic_fetch_add(&progress->sequence, 1);
  atomic_store(&progress->completed, 0);
  atomic_store(&progress->total, total);
  atomic_store(&progress->stage, stage);
  atomic_fetch_add(&progress->sequence, 1);
}

static inline void wmp_build_progress_snapshot(const WMPBuildProgress *progress,
                                               int *stage, int *completed,
                                               int *total) {
  while (true) {
    const unsigned int sequence = atomic_load(&progress->sequence);
    if ((sequence & 1U) != 0) {
      continue;
    }
    *stage = atomic_load(&progress->stage);
    *completed = atomic_load(&progress->completed);
    *total = atomic_load(&progress->total);
    if (sequence == atomic_load(&progress->sequence)) {
      return;
    }
  }
}

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