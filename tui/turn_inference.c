#include "turn_inference.h"

#include "../src/def/inference_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"
#include "../src/ent/stats.h"
#include "../src/util/math_util.h"
#include "game_state.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool tui_turn_inference_applies(const TuiGameState *state, int turn_idx) {
  if (turn_idx < 1 || turn_idx >= state->history_count) {
    return false;
  }
  const TuiHistoryEntry *entry = &state->history[turn_idx];
  const TuiHistoryEntry *prev = &state->history[turn_idx - 1];
  if (entry->kind != TUI_HISTORY_ENTRY_MOVE ||
      prev->kind != TUI_HISTORY_ENTRY_MOVE || prev->pending ||
      prev->player_idx == entry->player_idx || prev->challenged_off ||
      prev->cgp_before[0] == '\0' || prev->move_str[0] == '\0' ||
      strcmp(prev->move_str, "pass") == 0) {
    return false;
  }
  // With the bag empty the opponent holds exactly the unseen tiles.
  return !entry->bag_before_known || entry->bag_before > 0;
}

// Sums, over every leave of `remaining` more tiles drawn from `pool`'s
// letters `ml` and up, the leave's value times its number of draws (the
// product of choose(pool count, count) over its letters), adding to
// *value_sum and *weight_sum. `leave` holds the letters chosen so far.
static void sum_random_leave_values(const KLV *klv, const Rack *pool,
                                    int ld_size, MachineLetter ml,
                                    int remaining, double draws, Rack *leave,
                                    double *value_sum, double *weight_sum) {
  if (remaining == 0) {
    *value_sum += draws * equity_to_double(klv_get_leave_value(klv, leave));
    *weight_sum += draws;
    return;
  }
  if (ml >= ld_size) {
    return;
  }
  const int available = rack_get_letter(pool, ml);
  const int most = available < remaining ? available : remaining;
  for (int count = 0; count <= most; count++) {
    if (count > 0) {
      rack_add_letter(leave, ml);
    }
    sum_random_leave_values(
        klv, pool, ld_size, (MachineLetter)(ml + 1), remaining - count,
        draws * (double)choose((uint64_t)available, (uint64_t)count), leave,
        value_sum, weight_sum);
  }
  for (int count = 0; count < most; count++) {
    rack_take_letter(leave, ml);
  }
}

void tui_inference_summary_fill(InferenceResults *results,
                                const LetterDistribution *ld, const KLV *klv,
                                int target_player_idx, const char *after_move,
                                TuiInferenceSummary *out) {
  memset(out, 0, sizeof(*out));
  const Stat *leave_values =
      inference_results_get_equity_values(results, INFERENCE_TYPE_LEAVE);
  const uint64_t racks = stat_get_num_samples(leave_values);
  if (racks == 0) {
    return;
  }
  out->target_player_idx = target_player_idx;
  (void)snprintf(out->after_move, sizeof(out->after_move), "%s", after_move);
  out->racks = racks;
  out->leave_value = stat_get_mean(leave_values);

  const int ld_size = ld_get_size(ld);
  const Rack *pool = inference_results_get_bag_as_rack(results);
  const int pool_total = rack_get_total_letters(pool);
  Stat *letter_stat = stat_create(false);
  double kept = 0.0;
  for (int ml = 0; ml < ld_size; ml++) {
    inference_results_set_stat_for_letter(results, INFERENCE_TYPE_LEAVE,
                                          letter_stat, (MachineLetter)ml);
    out->letter_mean[ml] = stat_get_mean(letter_stat);
    kept += out->letter_mean[ml];
  }
  stat_destroy(letter_stat);
  // Every inferred leave keeps the same number of tiles.
  out->kept = (int)lround(kept);
  if (out->kept <= 0 || pool_total < out->kept) {
    return;
  }
  for (int ml = 0; ml < ld_size; ml++) {
    out->letter_random[ml] = (double)out->kept *
                             (double)rack_get_letter(pool, (MachineLetter)ml) /
                             (double)pool_total;
    const double face = equity_to_double(ld_get_score(ld, (MachineLetter)ml));
    out->face_value += out->letter_mean[ml] * face;
    out->face_value_random += out->letter_random[ml] * face;
    if (ld_get_is_vowel(ld, (MachineLetter)ml)) {
      out->vowels += out->letter_mean[ml];
      out->vowels_random += out->letter_random[ml];
    }
  }
  Rack leave;
  rack_set_dist_size_and_reset(&leave, ld_size);
  double value_sum = 0.0;
  double weight_sum = 0.0;
  sum_random_leave_values(klv, pool, ld_size, 0, out->kept, 1.0, &leave,
                          &value_sum, &weight_sum);
  out->leave_value_random = weight_sum > 0.0 ? value_sum / weight_sum : 0.0;
  out->valid = true;
}
