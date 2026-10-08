#include "pat_eval.h"

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/pat_defs.h"
#include "../def/rack_defs.h"
#include "board.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
#include "pat.h"
#include "pat_features.h"
#include "pat_utility.h"
#include "rack.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static_assert(PAT_NUM_FEATURES <= UINT8_MAX + 1,
              "group feature indexes are bytes");

// The unit group of a context unit (see PAT_NUM_UNIT_GROUPS).
static int pat_unit_group(const PATEvalContext *pat_eval_ctx, int unit_index) {
  const int num_premium_units = pat_eval_ctx->num_premium * 2;
  if (unit_index < num_premium_units) {
    return pat_eval_ctx->premium_classes[unit_index / 2];
  }
  return PAT_NUM_PREMIUM_CLASSES +
         pat_eval_ctx->dd_tiers[unit_index - num_premium_units];
}

// The weights' product with a unit's feature row, over only the weighted
// features the unit's group can write; the row's other terms are zero.
// Every path that has a context uses this one.
static Equity pat_dot_unit(const PATEvalContext *pat_eval_ctx, int unit_index,
                           const int32_t *features) {
  const int unit_group = pat_unit_group(pat_eval_ctx, unit_index);
  const uint8_t *feature_index = pat_eval_ctx->group_feature_index[unit_group];
  const Equity *feature_weight = pat_eval_ctx->group_feature_weight[unit_group];
  const int num_features = pat_eval_ctx->group_num_features[unit_group];
  int64_t acc = 0;
  for (int group_idx = 0; group_idx < num_features; group_idx++) {
    acc +=
        (int64_t)feature_weight[group_idx] * features[feature_index[group_idx]];
  }
  const Equity penalty = pat_clamp_dot(acc);
  assert(penalty == pat_dot(pat_eval_ctx->weights, features));
  return penalty;
}

// The per-row and per-column unit masks are 64-bit.
static_assert(PAT_MASK_WORDS >= 1, "unit masks need at least one word");

// The affected-unit set outgrew one word when the lesser premium squares
// joined the scan, so it is a small fixed bitset. All of these are hot: the
// per-move path builds one and walks its bits.
static inline void pat_mask_clear(uint64_t *mask) {
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    mask[word] = 0;
  }
}

static inline void pat_mask_set(uint64_t *mask, int unit_index) {
  mask[unit_index / 64] |= (uint64_t)1 << (unit_index % 64);
}

static inline bool pat_mask_test(const uint64_t *mask, int unit_index) {
  return (mask[unit_index / 64] >> (unit_index % 64)) & 1;
}

static inline void pat_mask_or_into(uint64_t *dst, const uint64_t *src) {
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    dst[word] |= src[word];
  }
}

static inline bool pat_mask_is_empty(const uint64_t *mask) {
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    if (mask[word] != 0) {
      return false;
    }
  }
  return true;
}

// Combines the per-unit penalties into the position's defense term. The
// opponent plays one move next turn, so two open lanes are not two separate
// losses: the worst route is charged in full and every other route at
// gamma, which is a convex combination of the minimum and the sum. Gamma 1
// is the plain sum. Being a convex combination of non-positive values the
// result is non-positive, and it is nondecreasing in every unit penalty,
// which is what lets the shadow bound below zero out the units a move can
// reach.
static Equity pat_combine(int64_t worst, int64_t sum, double combine_gamma) {
  double combined =
      (1.0 - combine_gamma) * (double)worst + combine_gamma * (double)sum;
  if (combined > 0.0) {
    combined = 0.0;
  }
  if (combined < (double)EQUITY_MIN_VALUE) {
    combined = (double)EQUITY_MIN_VALUE;
  }
  return (Equity)llround(combined);
}

static Equity pat_combine_unit_penalties(const Equity *unit_penalties,
                                         int num_units, double combine_gamma) {
  int64_t sum = 0;
  int64_t worst = 0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    const int64_t penalty = unit_penalties[unit_index];
    sum += penalty;
    if (penalty < worst) {
      worst = penalty;
    }
  }
  return pat_combine(worst, sum, combine_gamma);
}

// The moves affecting the units can at best zero out each affected unit's (<=
// 0) baseline contribution; every unaffected unit keeps its baseline exactly.
static Equity pat_units_penalty_bound(const PATEvalContext *pat_eval_ctx,
                                      const uint64_t *affected_units) {
  // A move can at best zero out every unit it reaches, and the combination
  // is nondecreasing in each unit, so combining with those units at zero
  // bounds the term from above. A zeroed unit adds nothing to the sum and
  // can never be the worst, so it simply drops out of both: the sum is the
  // total less the reached baselines, and the worst is the first unit in
  // penalty order the move does not reach. Only the reached units are
  // visited, and a move reaches few.
  int64_t sum = pat_eval_ctx->total_unit_penalty;
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    uint64_t bits = affected_units[word];
    while (bits != 0) {
      const int unit_index = word * 64 + pat_ctz(bits);
      sum -= pat_eval_ctx->unit_penalty[unit_index];
      bits &= bits - 1;
    }
  }
  int64_t worst = 0;
  for (int order_idx = 0; order_idx < pat_eval_ctx->num_units; order_idx++) {
    const int unit_index = pat_eval_ctx->units_by_penalty[order_idx];
    if (!pat_mask_test(affected_units, unit_index)) {
      worst = pat_eval_ctx->unit_penalty[unit_index];
      break;
    }
  }
  return pat_combine(worst, sum, pat_eval_ctx->weights->combine_gamma);
}

void pat_eval_context_disable(PATEvalContext *pat_eval_ctx) {
  pat_eval_ctx->weights = NULL;
  pat_eval_ctx->utility_row = NULL;
  pat_eval_ctx->utility_bound = 0;
  pat_eval_ctx->utility_non_placement = 0;
}

void pat_eval_context_set_utility(PATEvalContext *pat_eval_ctx, int margin,
                                  int bag) {
  const PATWeights *weights = pat_eval_ctx->weights;
  if (!weights || !weights->utility_table) {
    return;
  }
  pat_eval_ctx->utility_margin = margin;
  pat_eval_ctx->utility_bag = bag;
  pat_eval_ctx->utility_row = weights->utility_table;
  pat_eval_ctx->utility_non_placement =
      weights->utility_table[pat_utility_index(weights, bag, margin)];
  // Every move adds a nonnegative score and draws 0 to RACK_SIZE tiles, so
  // the largest correction any move can get is the largest suffix maximum
  // from the current margin over those bag sizes.
  Equity bound = pat_eval_ctx->utility_non_placement;
  for (int drawn = 0; drawn <= RACK_SIZE; drawn++) {
    const int bag_after = bag > drawn ? bag - drawn : 0;
    const Equity suffix_max =
        weights
            ->utility_suffix_max[pat_utility_index(weights, bag_after, margin)];
    if (suffix_max > bound) {
      bound = suffix_max;
    }
  }
  pat_eval_ctx->utility_bound = bound;
}

// The utility correction for move (see PAT_UTILITY_ADJUST_ROW_PREFIX), 0
// when the context has none.
static inline Equity
pat_eval_utility_adjustment(const PATEvalContext *pat_eval_ctx,
                            const Move *move) {
  if (!pat_eval_ctx->utility_row) {
    return 0;
  }
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return pat_eval_ctx->utility_non_placement;
  }
  const int bag = pat_eval_ctx->utility_bag;
  const int drawn = move_get_tiles_played(move);
  const int bag_after = bag > drawn ? bag - drawn : 0;
  const int margin =
      pat_eval_ctx->utility_margin + equity_to_int(move_get_score(move));
  return pat_eval_ctx->utility_row[pat_utility_index(pat_eval_ctx->weights,
                                                     bag_after, margin)];
}

// Scans one of the context's units into `features`, reporting the lane it
// walks and the span of lane squares that walk could read. Units 2*i and
// 2*i+1 are premium i's horizontal and vertical walks; the units after those
// are the double-double windows.
static void pat_scan_context_unit(const PATEvalContext *pat_eval_ctx,
                                  int unit_index, const PATMoveOverlay *overlay,
                                  int32_t *features, int *dir_out,
                                  int *lane_out, int *extent_lo, int *extent_hi,
                                  uint64_t *hook_letters_out) {
  const int num_premium_units = pat_eval_ctx->num_premium * 2;
  if (unit_index < num_premium_units) {
    const int premium_idx = unit_index / 2;
    const int dir = unit_index % 2;
    const int premium_row = pat_eval_ctx->premium_rows[premium_idx];
    const int premium_col = pat_eval_ctx->premium_cols[premium_idx];
    *dir_out = dir;
    *lane_out = (dir == BOARD_HORIZONTAL_DIRECTION) ? premium_row : premium_col;
    pat_scan_unit(
        pat_eval_ctx->lanes, pat_eval_ctx->ld, pat_eval_ctx->unseen_counts,
        pat_eval_ctx->weights, premium_row, premium_col,
        pat_eval_ctx->premium_classes[premium_idx], dir, overlay, features,
        extent_lo, extent_hi, pat_eval_ctx->opponent_rack_size,
        hook_letters_out, pat_eval_ctx->score_channels);
    return;
  }
  const int dd_idx = unit_index - num_premium_units;
  *dir_out = pat_eval_ctx->dd_dirs[dd_idx];
  *lane_out = pat_eval_ctx->dd_lanes[dd_idx];
  pat_scan_dd_unit(pat_eval_ctx->lanes, pat_eval_ctx->unseen_counts,
                   pat_eval_ctx->dd_dirs[dd_idx],
                   pat_eval_ctx->dd_lanes[dd_idx], pat_eval_ctx->dd_los[dd_idx],
                   pat_eval_ctx->dd_his[dd_idx], pat_eval_ctx->dd_tiers[dd_idx],
                   overlay, features, extent_lo, extent_hi,
                   pat_eval_ctx->opponent_rack_size, hook_letters_out);
}

// Whether any channel a walk from this premium class can write carries a
// nonzero weight and the runtime mask has not excluded the class outright.
// Each class writes only its own hook and floater-value channels; the
// triple word class alone also writes the flexibility, through-table and
// triple-triple channels.
static bool pat_class_is_weighted(const PATWeights *weights, int premium_class,
                                  uint32_t enabled_classes_mask) {
  if (!(enabled_classes_mask & (1U << premium_class))) {
    return false;
  }
  int start = 0;
  int end = 0;
  switch (premium_class) {
  case PAT_PREMIUM_TWS:
    start = PAT_FEATURE_HOOK_START;
    end = PAT_FEATURE_DWS_HOOK_START;
    break;
  case PAT_PREMIUM_DWS:
    start = PAT_FEATURE_DWS_HOOK_START;
    end = PAT_FEATURE_TLS_HOOK_START;
    break;
  case PAT_PREMIUM_TLS:
    start = PAT_FEATURE_TLS_HOOK_START;
    end = PAT_FEATURE_DLS_HOOK_START;
    break;
  case PAT_PREMIUM_DLS:
    start = PAT_FEATURE_DLS_HOOK_START;
    end = PAT_FEATURE_QWS_HOOK_START;
    break;
  case PAT_PREMIUM_QWS:
    start = PAT_FEATURE_QWS_HOOK_START;
    end = PAT_FEATURE_QLS_HOOK_START;
    break;
  case PAT_PREMIUM_QLS:
    start = PAT_FEATURE_QLS_HOOK_START;
    end = PAT_FEATURE_TT_FLOATER;
    break;
  default:
    return true;
  }
  for (int feature_index = start; feature_index < end; feature_index++) {
    if (weights->weights[feature_index] != 0) {
      return true;
    }
  }
  return premium_class == PAT_PREMIUM_TWS &&
         (weights->weights[PAT_FEATURE_TT_FLOATER] != 0 ||
          weights->weights[PAT_FEATURE_TT_HOOK_ONLY] != 0);
}

// Whether any of a window tier's three channels carries a nonzero weight
// and the runtime mask has not excluded windows outright.
static bool pat_tier_is_weighted(const PATWeights *weights, int tier,
                                 uint32_t enabled_classes_mask) {
  if (!(enabled_classes_mask & PAT_CLASS_MASK_WINDOWS)) {
    return false;
  }
  const int start =
      PAT_FEATURE_WINDOW_START + tier * PAT_WINDOW_FEATURES_PER_TIER;
  for (int feature_index = start;
       feature_index < start + PAT_WINDOW_FEATURES_PER_TIER; feature_index++) {
    if (weights->weights[feature_index] != 0) {
      return true;
    }
  }
  return false;
}

// Drops the units whose every channel is unweighted. Such a unit's penalty
// is zero on any board, with or without a move overlaid, and a zero penalty
// is never the worst unit and adds nothing to the sum, so removing it
// changes no penalty, combination, or lane bound: it only saves the walk.
// The enumeration and its caps have already run, so which squares exist
// was decided exactly as training decides it.
static void pat_drop_unweighted_units(PATEvalContext *pat_eval_ctx,
                                      const PATWeights *weights,
                                      uint32_t enabled_classes_mask) {
  bool class_weighted[PAT_NUM_PREMIUM_CLASSES];
  for (int premium_class = 0; premium_class < PAT_NUM_PREMIUM_CLASSES;
       premium_class++) {
    class_weighted[premium_class] =
        pat_class_is_weighted(weights, premium_class, enabled_classes_mask);
  }
  int kept = 0;
  for (int premium_idx = 0; premium_idx < pat_eval_ctx->num_premium;
       premium_idx++) {
    if (!class_weighted[pat_eval_ctx->premium_classes[premium_idx]]) {
      continue;
    }
    pat_eval_ctx->premium_rows[kept] = pat_eval_ctx->premium_rows[premium_idx];
    pat_eval_ctx->premium_cols[kept] = pat_eval_ctx->premium_cols[premium_idx];
    pat_eval_ctx->premium_classes[kept] =
        pat_eval_ctx->premium_classes[premium_idx];
    kept++;
  }
  pat_eval_ctx->num_premium = kept;

  bool tier_weighted[PAT_WINDOW_TIER_COUNT];
  for (int tier = 0; tier < PAT_WINDOW_TIER_COUNT; tier++) {
    tier_weighted[tier] =
        pat_tier_is_weighted(weights, tier, enabled_classes_mask);
  }
  kept = 0;
  for (int dd_idx = 0; dd_idx < pat_eval_ctx->num_dd; dd_idx++) {
    if (!tier_weighted[pat_eval_ctx->dd_tiers[dd_idx]]) {
      continue;
    }
    pat_eval_ctx->dd_dirs[kept] = pat_eval_ctx->dd_dirs[dd_idx];
    pat_eval_ctx->dd_lanes[kept] = pat_eval_ctx->dd_lanes[dd_idx];
    pat_eval_ctx->dd_los[kept] = pat_eval_ctx->dd_los[dd_idx];
    pat_eval_ctx->dd_his[kept] = pat_eval_ctx->dd_his[dd_idx];
    pat_eval_ctx->dd_tiers[kept] = pat_eval_ctx->dd_tiers[dd_idx];
    kept++;
  }
  pat_eval_ctx->num_dd = kept;
}

static void pat_eval_context_load_units(
    PATEvalContext *pat_eval_ctx, const PATWeights *weights,
    const Square *lanes, const LetterDistribution *ld, const Rack *player_rack,
    bool drop_unweighted_units, uint32_t enabled_classes_mask,
    int opponent_rack_size) {
  pat_eval_ctx->weights = weights;
  // Inert until pat_eval_context_set_utility says otherwise.
  pat_eval_ctx->utility_row = NULL;
  pat_eval_ctx->utility_bound = 0;
  pat_eval_ctx->utility_non_placement = 0;
  if (!weights) {
    return;
  }
  pat_eval_ctx->opponent_rack_size = opponent_rack_size;
  pat_eval_ctx->active_classes_mask = 0;
  for (int premium_class = 0; premium_class < PAT_NUM_PREMIUM_CLASSES;
       premium_class++) {
    if (pat_class_is_weighted(weights, premium_class, enabled_classes_mask)) {
      pat_eval_ctx->active_classes_mask |= 1U << premium_class;
    }
  }
  pat_eval_ctx->ld = ld;
  pat_eval_ctx->lanes = lanes;
  pat_compute_unseen_counts(lanes, ld, player_rack,
                            pat_eval_ctx->unseen_counts);
  pat_eval_ctx->num_premium = pat_find_premium_squares(
      lanes, pat_eval_ctx->premium_rows, pat_eval_ctx->premium_cols,
      pat_eval_ctx->premium_classes);
  pat_eval_ctx->num_dd = pat_find_dd(
      lanes, pat_eval_ctx->dd_dirs, pat_eval_ctx->dd_lanes,
      pat_eval_ctx->dd_los, pat_eval_ctx->dd_his, pat_eval_ctx->dd_tiers);
  if (drop_unweighted_units) {
    pat_drop_unweighted_units(pat_eval_ctx, weights, enabled_classes_mask);
  }
  pat_eval_ctx->num_units =
      pat_eval_ctx->num_premium * 2 + pat_eval_ctx->num_dd;
  memset(pat_eval_ctx->unit_mask_by_row, 0,
         sizeof(pat_eval_ctx->unit_mask_by_row));
  memset(pat_eval_ctx->unit_mask_by_col, 0,
         sizeof(pat_eval_ctx->unit_mask_by_col));
  static_assert(PAT_MAX_SCAN_UNITS <= PAT_MASK_WORDS * 64,
                "unit masks must cover every scan unit");
  static_assert(PAT_MAX_SCAN_UNITS <= UINT16_MAX,
                "unit order entries must hold every unit index");
  pat_eval_ctx->board_is_empty = true;
  for (int row = 0; row < BOARD_DIM && pat_eval_ctx->board_is_empty; row++) {
    const Square *lane =
        board_get_row_cache(lanes, row, BOARD_HORIZONTAL_DIRECTION);
    for (int col = 0; col < BOARD_DIM; col++) {
      if (square_get_letter(&lane[col]) != ALPHABET_EMPTY_SQUARE_MARKER) {
        pat_eval_ctx->board_is_empty = false;
        break;
      }
    }
  }
  {
    int total_unseen = 0;
    for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
      total_unseen += pat_eval_ctx->unseen_counts[ml];
    }
    int bag_count = total_unseen - opponent_rack_size;
    if (bag_count < 0) {
      bag_count = 0;
    }
    pat_eval_ctx->term_scale =
        weights->stage_scale[pat_stage_for_bag(bag_count)];
  }
  pat_eval_ctx->score_channels = !drop_unweighted_units;
  for (int bin_idx = 0; bin_idx < PAT_HOOK_BIN_COUNT; bin_idx++) {
    if (weights->weights[PAT_FEATURE_HOOK_SCORE_START + bin_idx] != 0) {
      pat_eval_ctx->score_channels = true;
    }
  }
  for (int unit_group = 0; unit_group < PAT_NUM_UNIT_GROUPS; unit_group++) {
    uint8_t group_features[PAT_NUM_FEATURES];
    const int num_group_features =
        pat_unit_group_features(unit_group, group_features);
    int num_weighted = 0;
    for (int group_idx = 0; group_idx < num_group_features; group_idx++) {
      const int feature_index = group_features[group_idx];
      if (weights->weights[feature_index] != 0) {
        pat_eval_ctx->group_feature_index[unit_group][num_weighted] =
            (uint8_t)feature_index;
        pat_eval_ctx->group_feature_weight[unit_group][num_weighted] =
            weights->weights[feature_index];
        num_weighted++;
      }
    }
    pat_eval_ctx->group_num_features[unit_group] = (uint8_t)num_weighted;
  }
  for (int unit_index = 0; unit_index < pat_eval_ctx->num_units; unit_index++) {
    int32_t unit_features[PAT_NUM_FEATURES];
    memset(unit_features, 0, sizeof(unit_features));
    int dir = 0;
    int lane = 0;
    int extent_lo = 0;
    int extent_hi = 0;
    pat_eval_ctx->unit_hook_letters[unit_index] = 0;
    pat_scan_context_unit(pat_eval_ctx, unit_index, NULL, unit_features, &dir,
                          &lane, &extent_lo, &extent_hi,
                          &pat_eval_ctx->unit_hook_letters[unit_index]);
    pat_eval_ctx->unit_penalty[unit_index] =
        pat_dot_unit(pat_eval_ctx, unit_index, unit_features);
    // A move affects this unit only when it has a tile on or directly
    // beside the lane (perpendicular halo of one) within the span of
    // squares the baseline walk visited: squares beyond the walk's break
    // point are unreachable within the empty-square budget either way.
    // Farther effects (a move extending a distant perpendicular word
    // into a lane square's cross set) are deliberately ignored in the
    // per-move delta.
    uint64_t (*halo_masks)[PAT_MASK_WORDS] =
        (dir == BOARD_HORIZONTAL_DIRECTION) ? pat_eval_ctx->unit_mask_by_row
                                            : pat_eval_ctx->unit_mask_by_col;
    uint64_t (*extent_masks)[PAT_MASK_WORDS] =
        (dir == BOARD_HORIZONTAL_DIRECTION) ? pat_eval_ctx->unit_mask_by_col
                                            : pat_eval_ctx->unit_mask_by_row;
    for (int halo = lane - 1; halo <= lane + 1; halo++) {
      if (halo >= 0 && halo < BOARD_DIM) {
        pat_mask_set(halo_masks[halo], unit_index);
      }
    }
    for (int idx = extent_lo; idx <= extent_hi; idx++) {
      pat_mask_set(extent_masks[idx], unit_index);
    }
  }
  // Reverse index of unit_hook_letters: bit u of units_by_hook_letter[L] set
  // exactly when unit u's baseline scan found a live hook or floater route
  // admitting letter L. Built once per position so a move's own leave (at
  // most RACK_SIZE distinct letters) can find every unit it could exploit
  // in a handful of ORs, entirely independent of the move's placement.
  memset(pat_eval_ctx->units_by_hook_letter, 0,
         sizeof(pat_eval_ctx->units_by_hook_letter));
  for (int unit_index = 0; unit_index < pat_eval_ctx->num_units; unit_index++) {
    uint64_t remaining_letters =
        pat_eval_ctx->unit_hook_letters[unit_index] & ~(uint64_t)1;
    while (remaining_letters) {
      const int machine_letter = pat_ctz(remaining_letters);
      remaining_letters &= remaining_letters - 1;
      pat_mask_set(pat_eval_ctx->units_by_hook_letter[machine_letter],
                   unit_index);
    }
  }
  // The total and the penalty order that let a move's combination be
  // formed from the units it reaches alone (see pat_units_penalty_bound).
  // Insertion sort: a few dozen units, once per position.
  int64_t total_unit_penalty = 0;
  for (int unit_index = 0; unit_index < pat_eval_ctx->num_units; unit_index++) {
    const Equity penalty = pat_eval_ctx->unit_penalty[unit_index];
    total_unit_penalty += penalty;
    int order_idx = unit_index;
    while (
        order_idx > 0 &&
        pat_eval_ctx
                ->unit_penalty[pat_eval_ctx->units_by_penalty[order_idx - 1]] >
            penalty) {
      pat_eval_ctx->units_by_penalty[order_idx] =
          pat_eval_ctx->units_by_penalty[order_idx - 1];
      order_idx--;
    }
    pat_eval_ctx->units_by_penalty[order_idx] = (uint16_t)unit_index;
  }
  pat_eval_ctx->total_unit_penalty = total_unit_penalty;
  pat_eval_ctx->pre_penalty = pat_combine_unit_penalties(
      pat_eval_ctx->unit_penalty, pat_eval_ctx->num_units,
      weights->combine_gamma);
  // Every move's leave is some subset of the player's starting rack, so the
  // units ANY move from this position could earn leave credit for (see
  // pat_leave_affected_units) are a subset of the units reachable through
  // some letter the player's rack holds right now. lane_penalty_bound is
  // position-level, not move-level, so it cannot know which specific leave
  // a later move will keep; folding this worst case in uniformly keeps it a
  // sound upper bound for every move in the lane rather than only the ones
  // whose leave happens to be empty. Empty whenever the file carries no
  // discount, so that case's bound is identical to before this feature.
  pat_mask_clear(pat_eval_ctx->worst_case_leave_units);
  if (weights->own_asset_discount > 0.0 && player_rack) {
    bool rack_has_blank =
        rack_get_letter(player_rack, BLANK_MACHINE_LETTER) > 0;
    const int dist_size = rack_get_dist_size(player_rack);
    for (int ml = 1; ml < dist_size; ml++) {
      if (rack_get_letter(player_rack, ml) > 0) {
        pat_mask_or_into(pat_eval_ctx->worst_case_leave_units,
                         pat_eval_ctx->units_by_hook_letter[ml]);
      }
    }
    if (rack_has_blank) {
      for (int ml = 1; ml < MAX_ALPHABET_SIZE; ml++) {
        pat_mask_or_into(pat_eval_ctx->worst_case_leave_units,
                         pat_eval_ctx->units_by_hook_letter[ml]);
      }
    }
  }
  for (int lane = 0; lane < BOARD_DIM; lane++) {
    uint64_t row_bound_units[PAT_MASK_WORDS];
    uint64_t col_bound_units[PAT_MASK_WORDS];
    for (int word = 0; word < PAT_MASK_WORDS; word++) {
      row_bound_units[word] = pat_eval_ctx->unit_mask_by_row[lane][word] |
                              pat_eval_ctx->worst_case_leave_units[word];
      col_bound_units[word] = pat_eval_ctx->unit_mask_by_col[lane][word] |
                              pat_eval_ctx->worst_case_leave_units[word];
    }
    pat_eval_ctx->lane_penalty_bound[BOARD_HORIZONTAL_DIRECTION][lane] =
        pat_eval_scaled(pat_eval_ctx,
                        pat_units_penalty_bound(pat_eval_ctx, row_bound_units));
    pat_eval_ctx->lane_penalty_bound[BOARD_VERTICAL_DIRECTION][lane] =
        pat_eval_scaled(pat_eval_ctx,
                        pat_units_penalty_bound(pat_eval_ctx, col_bound_units));
  }
}

void pat_eval_context_set_kwg(PATEvalContext *pat_eval_ctx, const KWG *kwg) {
  pat_eval_ctx->kwg = kwg;
}

void pat_eval_context_load(PATEvalContext *pat_eval_ctx,
                           const PATWeights *weights, const Square *lanes,
                           const LetterDistribution *ld,
                           const Rack *player_rack,
                           uint32_t enabled_classes_mask,
                           int opponent_rack_size) {
  pat_require_prepared(weights);
  pat_eval_context_load_units(pat_eval_ctx, weights, lanes, ld, player_rack,
                              true, enabled_classes_mask, opponent_rack_size);
}

void pat_eval_context_load_all_units(PATEvalContext *pat_eval_ctx,
                                     const PATWeights *weights,
                                     const Square *lanes,
                                     const LetterDistribution *ld,
                                     const Rack *player_rack,
                                     int opponent_rack_size) {
  pat_eval_context_load_units(pat_eval_ctx, weights, lanes, ld, player_rack,
                              false, PAT_CLASS_MASK_ALL, opponent_rack_size);
}

// Returns the bitset of scan units the move can affect (see the
// unit_mask_by_row comment in the header).
static inline void pat_move_affected_units(const PATEvalContext *pat_eval_ctx,
                                           int row_start, int row_end,
                                           int col_start, int col_end,
                                           uint64_t *affected_units) {
  uint64_t row_units[PAT_MASK_WORDS];
  uint64_t col_units[PAT_MASK_WORDS];
  pat_mask_clear(row_units);
  pat_mask_clear(col_units);
  for (int row = row_start; row <= row_end; row++) {
    pat_mask_or_into(row_units, pat_eval_ctx->unit_mask_by_row[row]);
  }
  for (int col = col_start; col <= col_end; col++) {
    pat_mask_or_into(col_units, pat_eval_ctx->unit_mask_by_col[col]);
  }
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    affected_units[word] = row_units[word] & col_units[word];
  }
}

// Bit L (L != BLANK_MACHINE_LETTER) is set when the leave holds at least
// one unblanked tile of machine letter L; *has_blank_out reports whether
// the leave holds a blank, which could stand in for whatever letter a
// hook needs. A NULL leave (a caller with no leave to offer) reports an
// empty mask and no blank, which is exactly "this move's own leave helps
// nothing" -- safe by construction, not a special case below.
static uint64_t pat_leave_letter_mask(const Rack *leave, bool *has_blank_out) {
  *has_blank_out = false;
  uint64_t mask = 0;
  if (!leave) {
    return 0;
  }
  const int dist_size = rack_get_dist_size(leave);
  for (int ml = 0; ml < dist_size; ml++) {
    if (rack_get_letter(leave, ml) == 0) {
      continue;
    }
    if (ml == BLANK_MACHINE_LETTER) {
      *has_blank_out = true;
    } else {
      mask |= (uint64_t)1 << ml;
    }
  }
  return mask;
}

// Every unit reachable through some letter in letter_mask, via the reverse
// index (see units_by_hook_letter); every unit with any live route at all
// when has_blank, since a blank stands in for whatever letter a hook needs.
static void pat_units_from_letter_mask(const PATEvalContext *pat_eval_ctx,
                                       uint64_t letter_mask, bool has_blank,
                                       uint64_t *units_out) {
  pat_mask_clear(units_out);
  uint64_t remaining_letters = letter_mask;
  while (remaining_letters) {
    const int machine_letter = pat_ctz(remaining_letters);
    remaining_letters &= remaining_letters - 1;
    pat_mask_or_into(units_out,
                     pat_eval_ctx->units_by_hook_letter[machine_letter]);
  }
  if (has_blank) {
    for (int machine_letter = 1; machine_letter < MAX_ALPHABET_SIZE;
         machine_letter++) {
      pat_mask_or_into(units_out,
                       pat_eval_ctx->units_by_hook_letter[machine_letter]);
    }
  }
}

// Units the move's own leave could exploit itself (see unit_hook_letters),
// independent of whether its placement geometrically touches them. Empty
// whenever the file carries no discount, so that case matches every
// earlier file's behavior exactly -- byte for byte, not just numerically.
static void pat_leave_affected_units(const PATEvalContext *pat_eval_ctx,
                                     const Rack *leave,
                                     uint64_t *leave_units_out) {
  pat_mask_clear(leave_units_out);
  if (pat_eval_ctx->weights->own_asset_discount <= 0.0) {
    return;
  }
  bool leave_has_blank = false;
  const uint64_t leave_mask = pat_leave_letter_mask(leave, &leave_has_blank);
  pat_units_from_letter_mask(pat_eval_ctx, leave_mask, leave_has_blank,
                             leave_units_out);
}

// Bounds pat_eval_move_penalty(move, leave) without rescanning: the units the
// move can reach and the units its leave could exploit (see
// pat_leave_affected_units) can each move all the way to 0 at best, so both
// sets are zeroed for the bound. A NULL leave uses
// worst_case_leave_units, since assuming no credit would make the bound too
// small.
Equity pat_eval_move_penalty_bound(const PATEvalContext *pat_eval_ctx,
                                   const Move *move, const Rack *leave) {
  if (!pat_eval_ctx || !pat_eval_ctx->weights) {
    return 0;
  }
  // The utility correction is exact and cheap, so the bound carries it
  // as is rather than bounding it.
  const Equity utility = pat_eval_utility_adjustment(pat_eval_ctx, move);
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return pat_eval_scaled(pat_eval_ctx, pat_eval_ctx->pre_penalty) + utility;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int tiles_length = move_get_tiles_length(move);
  const int row_end = vertical ? row_start + tiles_length - 1 : row_start;
  const int col_end = vertical ? col_start : col_start + tiles_length - 1;
  uint64_t affected_units[PAT_MASK_WORDS];
  pat_move_affected_units(pat_eval_ctx, row_start, row_end, col_start, col_end,
                          affected_units);
  uint64_t leave_units[PAT_MASK_WORDS];
  if (leave) {
    pat_leave_affected_units(pat_eval_ctx, leave, leave_units);
  } else {
    for (int word = 0; word < PAT_MASK_WORDS; word++) {
      leave_units[word] = pat_eval_ctx->worst_case_leave_units[word];
    }
  }
  uint64_t combined_units[PAT_MASK_WORDS];
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    combined_units[word] = affected_units[word] | leave_units[word];
  }
  return pat_eval_scaled(pat_eval_ctx, pat_units_penalty_bound(
                                           pat_eval_ctx, combined_units)) +
         utility;
}

// The opening adjustment for a move on an empty board (see
// PAT_OPENING_TILES_ROW_PREFIX): a placement's entry for its tile count, and
// the exchange entry for both an exchange and a pass, which leave the board
// empty alike (so the table cannot break a static tie between them). Applies
// to either player while the board is empty; zero once a tile is down.
static inline Equity pat_opening_adjustment(const PATEvalContext *pat_eval_ctx,
                                            const Move *move) {
  if (!pat_eval_ctx->board_is_empty) {
    return 0;
  }
  if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return pat_eval_ctx->weights->opening_tiles[move_get_tiles_played(move)];
  }
  return pat_eval_ctx->weights->opening_exchange;
}

static Equity pat_eval_move_penalty_scaled(const PATEvalContext *pat_eval_ctx,
                                           const Move *move, const Rack *leave,
                                           Equity scaled_floor);

Equity pat_eval_move_penalty_capped(const PATEvalContext *pat_eval_ctx,
                                    const Move *move, const Rack *leave,
                                    Equity floor) {
  if (!pat_eval_ctx || !pat_eval_ctx->weights) {
    return 0;
  }
  // The opening adjustment is <= 0 like everything else here, so every
  // bound on the term stays a bound without knowing about it. The utility
  // correction is not; see pat_eval_utility_bound. Both are exact and
  // cheap, so the floor passes through them to the scaled term.
  const Equity adjustments = pat_opening_adjustment(pat_eval_ctx, move) +
                             pat_eval_utility_adjustment(pat_eval_ctx, move);
  Equity scaled_floor = EQUITY_MIN_VALUE;
  if (floor > EQUITY_MIN_VALUE) {
    int64_t needed = (int64_t)floor - adjustments;
    if (needed < EQUITY_MIN_VALUE) {
      needed = EQUITY_MIN_VALUE;
    } else if (needed > EQUITY_MAX_VALUE) {
      needed = EQUITY_MAX_VALUE;
    }
    scaled_floor = (Equity)needed;
  }
  return pat_eval_move_penalty_scaled(pat_eval_ctx, move, leave, scaled_floor) +
         adjustments;
}

Equity pat_eval_move_penalty(const PATEvalContext *pat_eval_ctx,
                             const Move *move, const Rack *leave) {
  return pat_eval_move_penalty_capped(pat_eval_ctx, move, leave,
                                      EQUITY_MIN_VALUE);
}

// The scaled term, rescanning the units the move reaches. Once an upper
// bound on the term falls below scaled_floor, returns that bound instead
// (see pat_eval_move_penalty_capped); EQUITY_MIN_VALUE never stops early,
// since no combination falls below it.
static Equity pat_eval_move_penalty_scaled(const PATEvalContext *pat_eval_ctx,
                                           const Move *move, const Rack *leave,
                                           Equity scaled_floor) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    // Exchanges and passes leave the board unchanged, so their defense term
    // is exactly the position baseline. Including it keeps the comparison
    // against tile placements (whose term is baseline plus delta) fair.
    return pat_eval_scaled(pat_eval_ctx, pat_eval_ctx->pre_penalty);
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int tiles_length = move_get_tiles_length(move);
  const int row_end = vertical ? row_start + tiles_length - 1 : row_start;
  const int col_end = vertical ? col_start : col_start + tiles_length - 1;
  uint64_t affected_units[PAT_MASK_WORDS];
  pat_move_affected_units(pat_eval_ctx, row_start, row_end, col_start, col_end,
                          affected_units);
  // Units the move's own leave could exploit, whether or not its placement
  // touches them (see unit_hook_letters); empty when the file carries no
  // discount. This uses the pre-move index, which is right for a unit the
  // move does not touch; a touched unit may have gained or lost hook letters,
  // so it is rechecked below against its overlay rescan.
  const double discount = pat_eval_ctx->weights->own_asset_discount;
  bool leave_has_blank = false;
  const uint64_t leave_mask =
      (discount > 0.0) ? pat_leave_letter_mask(leave, &leave_has_blank) : 0;
  uint64_t leave_units[PAT_MASK_WORDS];
  pat_leave_affected_units(pat_eval_ctx, leave, leave_units);
  uint64_t combined_units[PAT_MASK_WORDS];
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    combined_units[word] = affected_units[word] | leave_units[word];
  }
  if (pat_mask_is_empty(combined_units)) {
    return pat_eval_scaled(pat_eval_ctx, pat_eval_ctx->pre_penalty);
  }
  const PATMoveOverlay overlay = {
      .move = move,
      .row_start = row_start,
      .col_start = col_start,
      .row_end = row_end,
      .col_end = col_end,
      .vertical = vertical,
      .hook_flex = pat_eval_ctx->weights->hook_flex,
      .kwg =
          pat_eval_ctx->weights->exact_created_hooks ? pat_eval_ctx->kwg : NULL,
      .lanes = pat_eval_ctx->lanes,
      .run_kwg =
          pat_eval_ctx->weights->exact_fresh_runs ? pat_eval_ctx->kwg : NULL,
  };
  // Rescan only the units the move can reach (geometrically, or through
  // its own leave) and combine the whole set: the units in neither set
  // keep exactly the penalty they were loaded with, so the sum is the
  // unreached units' baselines plus each reached unit's new penalty, and
  // the worst is the lesser of the reached units' new penalties and the
  // first unreached unit in penalty order.
  //
  // A reached unit not rescanned yet counts as 0, the most the move can
  // gain there, so combining what is known bounds the term from above at
  // every step: pat_combine is nondecreasing in each unit, and scaling
  // keeps the order. With a floor, the reached units are rescanned worst
  // baseline first, which lowers that bound fastest, and the rescans stop
  // once it falls below the floor. The sum and the worst do not depend on
  // the order, so a finished scan gives the same term either way.
  const double combine_gamma = pat_eval_ctx->weights->combine_gamma;
  int64_t sum = pat_eval_ctx->total_unit_penalty;
  uint16_t reached_units[PAT_MAX_SCAN_UNITS];
  int num_reached = 0;
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    uint64_t bits = combined_units[word];
    while (bits != 0) {
      const int unit_index = word * 64 + pat_ctz(bits);
      bits &= bits - 1;
      sum -= pat_eval_ctx->unit_penalty[unit_index];
      reached_units[num_reached++] = (uint16_t)unit_index;
    }
  }
  int64_t worst = 0;
  for (int order_idx = 0; order_idx < pat_eval_ctx->num_units; order_idx++) {
    const int unit_index = pat_eval_ctx->units_by_penalty[order_idx];
    if (!pat_mask_test(combined_units, unit_index)) {
      if (pat_eval_ctx->unit_penalty[unit_index] < worst) {
        worst = pat_eval_ctx->unit_penalty[unit_index];
      }
      break;
    }
  }
  const bool has_floor = scaled_floor > EQUITY_MIN_VALUE;
  if (has_floor) {
    // Every reached unit at 0 (pat_eval_move_penalty_bound's bound).
    const Equity bound =
        pat_eval_scaled(pat_eval_ctx, pat_combine(worst, sum, combine_gamma));
    if (bound < scaled_floor) {
      return bound;
    }
    int num_ordered = 0;
    for (int order_idx = 0;
         order_idx < pat_eval_ctx->num_units && num_ordered < num_reached;
         order_idx++) {
      const int unit_index = pat_eval_ctx->units_by_penalty[order_idx];
      if (pat_mask_test(combined_units, unit_index)) {
        reached_units[num_ordered++] = (uint16_t)unit_index;
      }
    }
  }
  for (int reached_idx = 0; reached_idx < num_reached; reached_idx++) {
    const int unit_index = reached_units[reached_idx];
    Equity penalty;
    bool qualifies_for_credit;
    if (pat_mask_test(affected_units, unit_index)) {
      int32_t overlay_features[PAT_NUM_FEATURES] = {0};
      int scan_dir = 0;
      int scan_lane = 0;
      uint64_t fresh_hook_letters = 0;
      // The same rack the training label was built against: whatever the
      // player holds now, not the leave this move would keep. Training
      // opens its observation after the mover has drawn back to full, so
      // scoring a candidate against its leave would call every hook
      // uncontested in proportion to how many tiles the move played,
      // which is a penalty on bingos and nothing to do with hooks.
      pat_scan_context_unit(pat_eval_ctx, unit_index, &overlay,
                            overlay_features, &scan_dir, &scan_lane, NULL, NULL,
                            discount > 0.0 ? &fresh_hook_letters : NULL);
      penalty = pat_dot_unit(pat_eval_ctx, unit_index, overlay_features);
      // The move's own placement can create or destroy this unit's hook
      // letters (a newly hooked square, or covering one that existed at
      // baseline), so eligibility is re-checked against the fresh,
      // overlay-aware scan rather than trusted from the baseline reverse
      // index that built leave_units.
      qualifies_for_credit =
          discount > 0.0 &&
          (leave_has_blank ? (fresh_hook_letters != 0)
                           : (fresh_hook_letters & leave_mask) != 0);
    } else {
      // Reached only through the leave: the move's placement never
      // touched this unit, so its geometry (and hence its hook letters)
      // is exactly what was loaded, and the baseline membership test is
      // still correct.
      penalty = pat_eval_ctx->unit_penalty[unit_index];
      qualifies_for_credit = pat_mask_test(leave_units, unit_index);
    }
    if (qualifies_for_credit) {
      // p <= 0 and discount in [0, 1], so p * (1 - discount) always
      // lands in [p, 0]: the credit can shrink a penalty toward zero
      // but never past it, so no new shadow-pruning bound is needed.
      penalty = (Equity)lround((double)penalty * (1.0 - discount));
    }
    sum += penalty;
    if (penalty < worst) {
      worst = penalty;
    }
    if (has_floor && reached_idx + 1 < num_reached) {
      const Equity bound =
          pat_eval_scaled(pat_eval_ctx, pat_combine(worst, sum, combine_gamma));
      if (bound < scaled_floor) {
        return bound;
      }
    }
  }
  return pat_eval_scaled(pat_eval_ctx, pat_combine(worst, sum, combine_gamma));
}
