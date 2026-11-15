#include "pat_utility.h"

#include "../def/pat_defs.h"
#include "../def/rack_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "equity.h"
#include "letter_distribution.h"
#include "pat.h"
#include "win_pct.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// The default sim utility (win 1.0, spread 0.5, scale 100; see
// sim_utility_blend) of the mover at margin with bag tiles left after the
// move and the opponent on turn, both racks full.
static double pat_utility_mover_value(const WinPct *win_pcts, int margin,
                                      unsigned int bag) {
  const double win =
      1.0 - win_pct_get(win_pcts, -margin, bag, RACK_SIZE, RACK_SIZE);
  const double scaled_spread = margin / 100.0;
  const double spread_sigmoid =
      scaled_spread >= 0.0 ? 1.0 / (1.0 + exp(-scaled_spread))
                           : exp(scaled_spread) / (1.0 + exp(scaled_spread));
  return (2.0 / 3.0) * win + (1.0 / 3.0) * spread_sigmoid;
}

static void pat_clear_utility_tables(PATWeights *pat) {
  free(pat->utility_table);
  free(pat->utility_suffix_max);
  free(pat->utility_range_max);
  pat->utility_range_max = NULL;
  free(pat->utility_win_pct_name);
  pat->utility_table = NULL;
  pat->utility_suffix_max = NULL;
  pat->utility_win_pct_name = NULL;
  pat->utility_max_bag = 0;
}

static void pat_build_utility_tables(PATWeights *pat, const WinPct *win_pcts) {
  pat_clear_utility_tables(pat);
  const int max_bag = (int)win_pct_get_max_bag(win_pcts);
  const size_t size = (size_t)(max_bag + 1) * PAT_UTILITY_WIDTH;
  pat->utility_max_bag = max_bag;
  pat->utility_table = malloc_or_die(sizeof(Equity) * size);
  pat->utility_suffix_max = malloc_or_die(sizeof(Equity) * size);
  pat->utility_win_pct_name = string_duplicate(win_pct_get_name(win_pcts));
  const int step = PAT_UTILITY_KAPPA_STEP;
  for (int bag = 0; bag <= max_bag; bag++) {
    Equity *row = pat->utility_table + (size_t)bag * PAT_UTILITY_WIDTH;
    Equity *suffix = pat->utility_suffix_max + (size_t)bag * PAT_UTILITY_WIDTH;
    for (int offset = 0; offset < PAT_UTILITY_WIDTH; offset++) {
      const int margin = offset - PAT_UTILITY_MARGIN_LIMIT;
      const double lower =
          pat_utility_mover_value(win_pcts, margin - step, (unsigned)bag);
      const double middle =
          pat_utility_mover_value(win_pcts, margin, (unsigned)bag);
      const double upper =
          pat_utility_mover_value(win_pcts, margin + step, (unsigned)bag);
      const double slope = (upper - lower) / (2.0 * step);
      const double curvature = (upper - 2.0 * middle + lower) / (step * step);
      const double kappa = slope > 0.0 ? -curvature / slope : 0.0;
      row[offset] = double_to_equity(0.5 * pat->utility_adjust * kappa);
    }
    Equity running = row[PAT_UTILITY_WIDTH - 1];
    for (int offset = PAT_UTILITY_WIDTH - 1; offset >= 0; offset--) {
      if (row[offset] > running) {
        running = row[offset];
      }
      suffix[offset] = running;
    }
  }
  // Level 0 is the table; level k's entry is the larger of two of level
  // k - 1's, 2^(k - 1) apart, kept within the row.
  pat->utility_range_max =
      malloc_or_die(sizeof(Equity) * size * PAT_UTILITY_RANGE_LEVELS);
  memcpy(pat->utility_range_max, pat->utility_table, sizeof(Equity) * size);
  for (int level = 1; level < PAT_UTILITY_RANGE_LEVELS; level++) {
    const Equity *below = pat->utility_range_max + (size_t)(level - 1) * size;
    Equity *this_level = pat->utility_range_max + (size_t)level * size;
    const int half = 1 << (level - 1);
    for (int bag = 0; bag <= max_bag; bag++) {
      const size_t row_start = (size_t)bag * PAT_UTILITY_WIDTH;
      for (int offset = 0; offset < PAT_UTILITY_WIDTH; offset++) {
        const int other =
            offset + half < PAT_UTILITY_WIDTH ? offset + half : offset;
        const Equity first_half = below[row_start + offset];
        const Equity second_half = below[row_start + other];
        this_level[row_start + offset] =
            first_half > second_half ? first_half : second_half;
      }
    }
  }
}

void pat_set_utility_adjust(PATWeights *pat, double utility_adjust,
                            const WinPct *win_pcts) {
  pat_clear_utility_tables(pat);
  pat->utility_adjust = utility_adjust;
  if (utility_adjust > 0.0) {
    pat_build_utility_tables(pat, win_pcts);
  }
}

void pat_prepare_utility(PATWeights *pat, const char *data_paths,
                         const LetterDistribution *ld,
                         ErrorStack *error_stack) {
  if (pat->utility_adjust <= 0.0) {
    return;
  }
  char *win_pct_name = win_pct_get_default_name(ld_get_name(ld));
  if (pat->utility_table != NULL &&
      strings_equal(pat->utility_win_pct_name, win_pct_name)) {
    free(win_pct_name);
    return;
  }
  WinPct *win_pcts = win_pct_create(data_paths, win_pct_name, error_stack);
  if (error_stack_is_empty(error_stack)) {
    pat_build_utility_tables(pat, win_pcts);
  } else {
    error_stack_push(
        error_stack, ERROR_STATUS_PAT_UTILITY_WIN_PCT,
        get_formatted_string("PAT '%s' has a utility correction, which needs "
                             "win percentage table '%s'",
                             pat->name, win_pct_name));
  }
  win_pct_destroy(win_pcts);
  free(win_pct_name);
}
