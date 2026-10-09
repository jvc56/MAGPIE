#ifndef PAT_FEATURES_H
#define PAT_FEATURES_H

#include "../def/pat_defs.h"
#include "board.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
#include "pat.h"
#include "rack.h"
#include <stdbool.h>
#include <stdint.h>

// Feature extraction: the lane walks that measure the opponent's access to
// premium squares, shared by evaluation (pat_eval.h) and training
// (pat_gen.h).

static inline int pat_ctz(uint64_t bits) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
  return __builtin_ctzll(bits);
#else
  int count = 0;
  while (!(bits & 1)) {
    bits >>= 1;
    count++;
  }
  return count;
#endif
}

// Overlay describing the candidate move's fresh tiles on top of the
// pre-move board. hook_flex approximates the flexibility of hooks and
// floaters the move itself creates, whose real cross and extension sets do
// not exist yet, unless kwg (hooks) or run_kwg (floater runs) resolves them.
typedef struct PATMoveOverlay {
  const Move *move;
  int row_start;
  int col_start;
  int row_end;
  int col_end;
  bool vertical;
  const uint8_t *hook_flex;
  // Non-NULL when hooks the move creates are to be resolved exactly (see
  // pat_fresh_cross_set); the lane cache goes with it.
  const KWG *kwg;
  const Square *lanes;
  // Non-NULL when floater runs the move places a tile in are to be resolved
  // exactly (see PATWeights.exact_fresh_runs).
  const KWG *run_kwg;
} PATMoveOverlay;

// Fills unseen_counts (MAX_ALPHABET_SIZE entries) with the tiles neither on
// the board nor on player_rack (NULL: none).
void pat_compute_unseen_counts(const Square *lanes,
                               const LetterDistribution *ld,
                               const Rack *player_rack, uint8_t *unseen_counts);
// Lists the empty premium squares, their classes, and the double-double
// windows (see PATEvalContext); both return the count.
int pat_find_premium_squares(const Square *lanes, uint8_t *premium_rows,
                             uint8_t *premium_cols, uint8_t *premium_classes);
int pat_find_dd(const Square *lanes, uint8_t *dd_dirs, uint8_t *dd_lanes,
                uint8_t *dd_los, uint8_t *dd_his, uint8_t *dd_tiers);
// The first hook and floater-value features of a premium class's channels.
void pat_class_channel_bases(int premium_class, int *hook_base,
                             int *float_score_base);
// Lists in feature_indexes the features a scan of a unit in unit_group (see
// PAT_NUM_UNIT_GROUPS) can write, and returns how many. Every other feature
// of the unit's row stays zero, so a dot product over the group's features
// alone equals the full one.
int pat_unit_group_features(int unit_group, uint8_t *feature_indexes);
// Scans one premium square's walk along dir, or one window, adding its
// features to features; see the definitions for the parameters.
void pat_scan_unit(const Square *lanes, const LetterDistribution *ld,
                   const uint8_t *unseen_counts, const PATWeights *pat,
                   int premium_row, int premium_col, int premium_class, int dir,
                   const PATMoveOverlay *overlay, int32_t *features,
                   int *extent_lo, int *extent_hi, int opponent_rack_size,
                   uint64_t *hook_letters_out, bool score_channels);
void pat_scan_dd_unit(const Square *lanes, const uint8_t *unseen_counts,
                      int dir, int lane_index, int lo, int hi, int tier,
                      const PATMoveOverlay *overlay, int32_t *features,
                      int *extent_lo, int *extent_hi, int opponent_rack_size,
                      uint64_t *hook_letters_out);
// The weights' dot product with a feature row, and the clamp that keeps it
// in [EQUITY_MIN_VALUE, 0].
int64_t pat_dot_raw(const PATWeights *pat, const int32_t *features);
Equity pat_clamp_dot(int64_t acc);
Equity pat_dot(const PATWeights *pat, const int32_t *features);
// Extracts the feature vector for the board as it stands (no move overlay),
// for the context baseline and for training on post-move boards. features has
// PAT_NUM_FEATURES elements. pat supplies the per-letter tables the floater
// channels read; NULL zeroes those channels. opponent_rack_size is the tile
// count only; pass RACK_SIZE if unknown.
void pat_extract_features(const Square *lanes, const LetterDistribution *ld,
                          const Rack *player_rack, const PATWeights *pat,
                          int opponent_rack_size, int32_t *features);
// The feature row to regress on when per-unit penalties combine with a gamma
// below one: gamma times every unit plus the remaining (1 - gamma) of the unit
// the current weights rank worst, so its dot product with the weights is the
// combined term. Iterating training generations converges on the fixed point.
// features has PAT_NUM_FEATURES elements.
void pat_extract_features_combined(const Square *lanes,
                                   const LetterDistribution *ld,
                                   const Rack *player_rack,
                                   const PATWeights *pat,
                                   int opponent_rack_size, double *features);

#endif
