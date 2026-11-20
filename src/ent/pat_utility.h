#ifndef PAT_UTILITY_H
#define PAT_UTILITY_H

#include "../def/pat_defs.h"
#include "../util/io_util.h"
#include "letter_distribution.h"
#include "pat.h"
#include "win_pct.h"
#include <stddef.h>

// The utility correction (see PAT_UTILITY_ADJUST_ROW_PREFIX): its tables,
// built from a win percentage table and indexed by the bag after the move
// and the mover's margin.

enum {
  PAT_UTILITY_WIDTH = 2 * PAT_UTILITY_MARGIN_LIMIT + 1,
  // Levels of utility_range_max: spans of up to 2^(levels - 1) margins,
  // which must reach PAT_UTILITY_PREFIX_MAX_SCORE + 1.
  PAT_UTILITY_RANGE_LEVELS = 8,
};

// Sets the utility correction (see PAT_UTILITY_ADJUST_ROW_PREFIX) and builds
// its tables from win_pcts; 0 removes it.
void pat_set_utility_adjust(PATWeights *pat, double utility_adjust,
                            const WinPct *win_pcts);
// Builds the utility correction's tables (see PAT_UTILITY_ADJUST_ROW_PREFIX)
// from the letter distribution's win percentage table, winpct_<ld>, unless
// they already come from it. A no-op without a correction. Call it with
// pat_prepare_hook_flex whenever the weights are used with a distribution.
void pat_prepare_utility(PATWeights *pat, const char *data_paths,
                         const LetterDistribution *ld, ErrorStack *error_stack);
// Row and column of the tables for a move leaving bag tiles in the bag from
// margin.
static inline size_t pat_utility_index(const PATWeights *pat, int bag,
                                       int margin) {
  if (bag < 0) {
    bag = 0;
  }
  if (bag > pat->utility_max_bag) {
    bag = pat->utility_max_bag;
  }
  if (margin < -PAT_UTILITY_MARGIN_LIMIT) {
    margin = -PAT_UTILITY_MARGIN_LIMIT;
  }
  if (margin > PAT_UTILITY_MARGIN_LIMIT) {
    margin = PAT_UTILITY_MARGIN_LIMIT;
  }
  return (size_t)bag * PAT_UTILITY_WIDTH +
         (size_t)(margin + PAT_UTILITY_MARGIN_LIMIT);
}

// The largest correction in the row of bag over the margins from margin to
// margin + span, each clamped to the table as pat_utility_index clamps it.
static inline Equity pat_utility_range_max(const PATWeights *pat, int bag,
                                           int margin, int span) {
  const size_t first = pat_utility_index(pat, bag, margin);
  const size_t last = pat_utility_index(pat, bag, margin + span);
  const unsigned length = (unsigned)(last - first) + 1;
  // floor(log2(length)), length >= 1.
  const int level = 31 - __builtin_clz(length);
  const size_t table_size =
      (size_t)(pat->utility_max_bag + 1) * PAT_UTILITY_WIDTH;
  const Equity *level_table =
      pat->utility_range_max + (size_t)level * table_size;
  const Equity from_first = level_table[first];
  const Equity to_last = level_table[last + 1 - ((size_t)1 << level)];
  return from_first > to_last ? from_first : to_last;
}

#endif
