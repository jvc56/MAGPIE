#ifndef WMP_STATS_H
#define WMP_STATS_H

// Diagnostic instrumentation for movegen WMP hash-walk hotspots. Compiled in
// only when MAGPIE_WMP_STATS is defined at build time (e.g. via
// `make magpie BUILD=release WMP_STATS=1`). When the macro is undefined the
// macros below expand to no-ops and the extra counters disappear from the
// hot path.
//
// Counters are global _Atomic uint64_t with relaxed ordering so the overhead
// is small even when enabled, but only single-threaded runs should be trusted
// for measurement -- cross-thread contention on the same cache lines will
// distort the numbers.
//
// On process exit (atexit), wmp_stats_print() dumps a per-category, per-bucket
// table to stderr.

#include <stdatomic.h>
#include <stdint.h>

#ifdef MAGPIE_WMP_STATS

enum {
  // wmp_move_gen_check_nonplaythroughs_of_size: per-subrack wmp_get_word_entry
  // done at gen_load_position time. Bucket = word_length (== subrack size,
  // no playthrough). TOTAL counts every call, FOUND counts calls that
  // returned non-NULL.
  WMP_STATS_NP_CHECK_TOTAL,
  WMP_STATS_NP_CHECK_FOUND,

  // wmp_move_gen_check_playthrough_full_rack_existence: single
  // wmp_get_word_entry done at shadow_record time for the full-rack bingo
  // check when the RIT single-playthrough fast path can't handle it (multi
  // playthrough or no RIT coverage). Bucket = word_length.
  WMP_STATS_PT_FULL_RACK_TOTAL,
  WMP_STATS_PT_FULL_RACK_FOUND,

  // wmp_move_gen_get_subrack_words playthrough branch: wmp_get_word_entry
  // done at record time for each playthrough subrack being expanded into
  // words. Bucket = word_length.
  WMP_STATS_PT_SUBRACK_WORDS_TOTAL,
  WMP_STATS_PT_SUBRACK_WORDS_FOUND,

  // wmp_entry_write_words_to_buffer: actual word materialization at record
  // time. Cost is proportional to num_words returned. Bucket = word_length.
  // The ADD counter tracks sum of num_words. The CALLS counter tracks
  // invocation count.
  WMP_STATS_WORDS_WRITTEN_NP_CALLS,
  WMP_STATS_WORDS_WRITTEN_NP_SUM,
  WMP_STATS_WORDS_WRITTEN_PT_CALLS,
  WMP_STATS_WORDS_WRITTEN_PT_SUM,

  // shadow_record: denominator for understanding hit/miss rates. Bucket =
  // tiles_played (0..RACK_SIZE+1).
  WMP_STATS_SHADOW_RECORD_CALLS,

  // shadow_record RIT fast-path decision breakdown for the playthrough
  // branch (has_playthrough). Each shadow_record call that reaches the
  // playthrough block increments exactly one of these. Bucket =
  // tiles_played.
  WMP_STATS_SHADOW_FAST_PATH_TAKEN,
  WMP_STATS_SHADOW_FAST_PATH_BYPASS_NO_RIT_ENTRY,
  WMP_STATS_SHADOW_FAST_PATH_BYPASS_MULTI_PLAYTHROUGH,
  WMP_STATS_SHADOW_FAST_PATH_BYPASS_NO_COVERAGE,
  WMP_STATS_SHADOW_FAST_PATH_PRUNED,

  // Number of shadow_record calls that took the "full-rack multi-pt
  // fallback" branch (wmp_move_gen_check_playthrough_full_rack_existence).
  // Redundant with PT_FULL_RACK_TOTAL but bucketed by tiles_played here.
  WMP_STATS_SHADOW_FALLBACK_FULL_RACK,

  WMP_STATS_NUM_CATEGORIES,
};

enum { WMP_STATS_MAX_BUCKETS = 32 };

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern _Atomic uint64_t
    g_wmp_stats[WMP_STATS_NUM_CATEGORIES][WMP_STATS_MAX_BUCKETS];

static inline void wmp_stats_add_internal(int category, int bucket,
                                          uint64_t delta) {
  int bucket_clamped = bucket;
  if (bucket_clamped < 0) {
    bucket_clamped = 0;
  }
  if (bucket_clamped >= WMP_STATS_MAX_BUCKETS) {
    bucket_clamped = WMP_STATS_MAX_BUCKETS - 1;
  }
  atomic_fetch_add_explicit(&g_wmp_stats[category][bucket_clamped], delta,
                            memory_order_relaxed);
}

#define WMP_STATS_INC(category, bucket)                                        \
  wmp_stats_add_internal((category), (bucket), 1)
#define WMP_STATS_ADD(category, bucket, delta)                                 \
  wmp_stats_add_internal((category), (bucket), (uint64_t)(delta))

// Helper for classifying which RIT fast-path bypass reason applies when
// shadow_record is in the playthrough branch but the fast path didn't
// apply. Split out from move_gen.c so the three-way if/else ladder
// doesn't trip bugprone-branch-clone when all three bodies collapse to
// ((void)0) in non-instrumented builds.
#define WMP_STATS_INC_BYPASS_REASON(rit_entry_ptr, num_playthrough_tiles,     \
                                    tiles_played_value)                        \
  do {                                                                         \
    if ((rit_entry_ptr) == NULL) {                                             \
      WMP_STATS_INC(WMP_STATS_SHADOW_FAST_PATH_BYPASS_NO_RIT_ENTRY,           \
                    (tiles_played_value));                                     \
    } else if ((num_playthrough_tiles) != 1) {                                 \
      WMP_STATS_INC(WMP_STATS_SHADOW_FAST_PATH_BYPASS_MULTI_PLAYTHROUGH,      \
                    (tiles_played_value));                                     \
    } else {                                                                   \
      WMP_STATS_INC(WMP_STATS_SHADOW_FAST_PATH_BYPASS_NO_COVERAGE,            \
                    (tiles_played_value));                                     \
    }                                                                          \
  } while (0)

void wmp_stats_init(void);
void wmp_stats_print(void);

#else // MAGPIE_WMP_STATS

#define WMP_STATS_INC(category, bucket) ((void)0)
#define WMP_STATS_ADD(category, bucket, delta) ((void)0)
#define WMP_STATS_INC_BYPASS_REASON(rit_entry_ptr, num_playthrough_tiles,     \
                                    tiles_played_value)                        \
  ((void)0)

static inline void wmp_stats_init(void) {}
static inline void wmp_stats_print(void) {}

#endif

#endif
