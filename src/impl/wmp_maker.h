#ifndef WMP_MAKER_H
#define WMP_MAKER_H

#include "../def/wmp_defs.h"
#include "../ent/dictionary_word.h"
#include "../ent/kwg.h"
#include "../ent/wmp.h"
#include <stdbool.h>

// Optional progress for one build at a time. Snapshot reads and cancellation
// may run concurrently with the builder. Destroy only after the builder joins.
// A completed unit is one word-length group, not elapsed-time progress.
typedef struct WMPBuildProgress WMPBuildProgress;
WMPBuildProgress *wmp_build_progress_create(void);
void wmp_build_progress_destroy(WMPBuildProgress *progress);
void wmp_build_progress_cancel(WMPBuildProgress *progress);
bool wmp_build_progress_is_cancelled(const WMPBuildProgress *progress);
void wmp_build_progress_set_stage(WMPBuildProgress *progress,
                                  wmp_build_stage_t stage, int total);
void wmp_build_progress_snapshot(WMPBuildProgress *progress,
                                 wmp_build_stage_t *stage, int *completed,
                                 int *total);

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