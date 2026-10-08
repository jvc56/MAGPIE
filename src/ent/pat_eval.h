#ifndef PAT_EVAL_H
#define PAT_EVAL_H

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/pat_defs.h"
#include "board.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
#include "pat.h"
#include "rack.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

// Static evaluation with a PATWeights: the per-position context and the
// per-move defense term.

// Per-position evaluation state, rebuilt by each movegen position load (and
// on the stack for validated moves). Holds the position-constant part of the
// defense term (pre_penalty, the penalty for the board as it stands) plus
// what the per-move delta needs to stay off the hot path: per scan unit (a
// premium square's row or column walk, or a window), the baseline features
// and the squares the walk visited, so a candidate move rescans a unit only
// when it places a tile on or beside a square that walk could see.
//
// The lanes pointer is only valid while the board is alive, untransposed,
// and unmutated, which holds for the duration of a move generation call and
// for a stack-scoped validated-move evaluation.
typedef struct PATEvalContext {
  // NULL means the context is disabled and the defense term is zero.
  const PATWeights *weights;
  const LetterDistribution *ld;
  const Square *lanes;
  // The lexicon for resolving hooks a move creates exactly (see
  // PATWeights.exact_created_hooks); NULL, the default after a load, means
  // the approximation is used. Set with pat_eval_context_set_kwg.
  const KWG *kwg;
  Equity pre_penalty;
  // The premium squares walked, in row-major order within each class, and
  // which class each belongs to.
  int num_premium;
  uint8_t premium_rows[PAT_MAX_PREMIUM];
  uint8_t premium_cols[PAT_MAX_PREMIUM];
  uint8_t premium_classes[PAT_MAX_PREMIUM];
  // Double-double windows: window i runs along lane dd_lanes[i] of
  // direction dd_dirs[i], from lane square dd_los[i] to dd_his[i], whose
  // squares are both double word squares.
  int num_dd;
  uint8_t dd_dirs[PAT_MAX_DD];
  uint8_t dd_lanes[PAT_MAX_DD];
  uint8_t dd_los[PAT_MAX_DD];
  uint8_t dd_his[PAT_MAX_DD];
  // Which product tier each window's two multipliers put it in.
  uint8_t dd_tiers[PAT_MAX_DD];
  // Units 2*i and 2*i+1 are premium i's horizontal and vertical walks; the
  // num_dd units after those are the double-double windows in order.
  int num_units;
  // How many tiles of each letter the opponent could still be holding: the
  // full distribution less the board and less the evaluating player's own
  // rack. Hook flexibility is measured against this rather than by counting
  // the letters a cross set admits, so a hook the opponent has no tile for
  // is no threat, and one only the evaluating player can fill is none
  // either. The player's whole rack is excluded, not the leave the move
  // would keep, since the context is built once for the position.
  uint8_t unseen_counts[MAX_ALPHABET_SIZE];
  // Each unit's baseline contribution to pre_penalty (always <= 0), used
  // to bound a move's penalty from above without rescanning.
  Equity unit_penalty[PAT_MAX_SCAN_UNITS];
  // Sum of every unit's baseline penalty, and the units ordered from the
  // most negative baseline up. A move's combination is then formed from
  // the units it reaches alone: the unreached sum is the total less the
  // reached baselines, and the unreached worst is the first unit in this
  // order the move does not reach.
  int64_t total_unit_penalty;
  uint16_t units_by_penalty[PAT_MAX_SCAN_UNITS];
  // For each unit group (see PAT_NUM_UNIT_GROUPS), the features its scans
  // can write that carry a nonzero weight, and those weights, so a unit's
  // dot product visits only those; every other term is exactly zero.
  uint8_t group_feature_index[PAT_NUM_UNIT_GROUPS][PAT_NUM_FEATURES];
  Equity group_feature_weight[PAT_NUM_UNIT_GROUPS][PAT_NUM_FEATURES];
  uint8_t group_num_features[PAT_NUM_UNIT_GROUPS];
  // Whether scans compute the hook-score channels: always for training
  // rows; at runtime only when a hook-score weight is nonzero.
  bool score_channels;
  // The stage factor for this position (see PATWeights.stage_scale),
  // applied to every penalty and bound the context hands out; 1.0 unless
  // the file carries a stage row. The stage is read off the unseen
  // counts: unseen is the bag plus the opponent's rack, so the pre-move
  // bag is unseen less opponent_rack_size.
  double term_scale;
  // Whether the position's board is empty, so the opening adjustments
  // (PATWeights.opening_tiles / opening_exchange) apply to its moves.
  bool board_is_empty;
  // Bit u of unit_mask_by_row[r] is set when a fresh tile in row r could
  // affect unit u provided the move's column span also overlaps the unit
  // (and symmetrically for columns), so a candidate move's affected-unit
  // set is the AND of the OR of its rows' masks with the OR of its
  // columns' masks: a few operations per candidate on the movegen record
  // path, zero for the vast majority of moves.
  uint64_t unit_mask_by_row[BOARD_DIM][PAT_MASK_WORDS];
  uint64_t unit_mask_by_col[BOARD_DIM][PAT_MASK_WORDS];
  // lane_penalty_bound[dir][lane] is an upper bound on the defense term of
  // every tile placement along that lane (a row for horizontal moves, a
  // column for vertical ones): the baseline penalty less the baseline
  // contribution of every unit a move in the lane could affect, each of
  // which it can at best zero out. Shadow pruning adds it to its per-anchor
  // equity bounds, which otherwise omit the term entirely; it is always
  // <= 0, so the bounds stay valid, and it is a plain array lookup so the
  // shadow hot path pays nothing for it.
  Equity lane_penalty_bound[2][BOARD_DIM];
  // Utility correction (see PAT_UTILITY_ADJUST_ROW_PREFIX), set per
  // position by pat_eval_context_set_utility; utility_row is NULL (and the
  // other two 0) when the weights carry none or it was not set.
  const Equity *utility_row;
  int utility_margin;
  int utility_bag;
  // The correction for an exchange or pass, and the largest any move can
  // get this position (folded into every movegen bound).
  Equity utility_non_placement;
  Equity utility_bound;
  // Bitmask of PAT_CLASS_MASK_* classes this context actually applies
  // (weighted in the file and not excluded by the runtime mask); see
  // pat_eval_context_get_active_classes, the safe way to read this from outside
  // pat_eval.c since it is meaningless (and left unset) while weights is NULL.
  uint32_t active_classes_mask;
  // The opponent's rack size (public), never its contents. A route needing
  // more fresh tiles than the opponent holds cannot be played, so scans cap
  // distance at this. PAT only runs while the bag has tiles, when every rack
  // is full, so this is RACK_SIZE today.
  int opponent_rack_size;
  // Bit L of unit_hook_letters[u] is set when some live hook or floater
  // route found by unit u's baseline scan accepts machine letter L (blanks
  // excluded, as in pat_set_flex), so a move's leave can be checked against
  // the letters that would let it exploit the unit whether or not the move
  // touches it. Computed once per position load.
  uint64_t unit_hook_letters[PAT_MAX_SCAN_UNITS];
  // Reverse index of the above: bit u of units_by_hook_letter[L] is set when
  // unit_hook_letters[u] has bit L, so the units a leave could exploit are a
  // few array reads away.
  uint64_t units_by_hook_letter[MAX_ALPHABET_SIZE][PAT_MASK_WORDS];
  // Units reachable through some letter the player's rack holds (every unit
  // with a live route, if the rack has a blank): a superset of what any
  // move's leave could exploit, since every leave is a subset of the rack.
  // Empty when the file carries no discount. It widens lane_penalty_bound
  // and is pat_eval_move_penalty_bound's fallback when a caller has no leave,
  // so a bound without leave information is never too small.
  uint64_t worst_case_leave_units[PAT_MASK_WORDS];
} PATEvalContext;

void pat_eval_context_disable(PATEvalContext *pat_eval_ctx);
// Sets the position the utility correction reads: the mover's lead before
// the move, in points, and the tiles in the bag. A no-op when the weights
// carry no correction; every load or disable clears it.
void pat_eval_context_set_utility(PATEvalContext *pat_eval_ctx, int margin,
                                  int bag);
void pat_eval_context_set_kwg(PATEvalContext *pat_eval_ctx, const KWG *kwg);
// Builds the context static evaluation uses. Premium squares whose class has
// no nonzero weight or that enabled_classes_mask excludes, and windows whose
// tier has none or whose PAT_CLASS_MASK_WINDOWS bit is clear, are not walked:
// their penalty would be zero. opponent_rack_size is the tile count only;
// pass RACK_SIZE if unknown.
void pat_eval_context_load(PATEvalContext *pat_eval_ctx,
                           const PATWeights *weights, const Square *lanes,
                           const LetterDistribution *ld,
                           const Rack *player_rack,
                           uint32_t enabled_classes_mask,
                           int opponent_rack_size);
// The same context with every unit walked whatever its weights: the
// reference the pruned context is checked against.
void pat_eval_context_load_all_units(PATEvalContext *pat_eval_ctx,
                                     const PATWeights *weights,
                                     const Square *lanes,
                                     const LetterDistribution *ld,
                                     const Rack *player_rack,
                                     int opponent_rack_size);
// Returns the PAT term for the move: the defense penalty for the opponent's
// access after the move (always <= 0) plus any opening adjustment and utility
// correction. Returns 0 when the context is NULL or disabled. Non-placement
// moves get the position baseline. leave is the tiles the move keeps, used
// only to credit units the leave could exploit (see
// PATWeights.own_asset_discount); NULL forgoes the credit.
Equity pat_eval_move_penalty(const PATEvalContext *pat_eval_ctx,
                             const Move *move, const Rack *leave);
// pat_eval_move_penalty(move, leave) when that is at least floor. Otherwise
// a value below floor that is still no less than the exact term: the lane
// rescans stop once an upper bound on the term falls below floor, and that
// bound is returned. So in a comparison against floor the result decides
// exactly as the exact term would; nothing else about it is exact.
// EQUITY_MIN_VALUE always returns the exact term.
Equity pat_eval_move_penalty_capped(const PATEvalContext *pat_eval_ctx,
                                    const Move *move, const Rack *leave,
                                    Equity floor);
// Returns an upper bound on pat_eval_move_penalty for the move without any
// lane rescans: the baseline penalty minus the baseline contributions of
// the units the move can affect or its leave could exploit (each of which
// the move can at best zero out). Used to skip the exact computation for
// moves that cannot contend. leave has the same meaning as in
// pat_eval_move_penalty above.
Equity pat_eval_move_penalty_bound(const PATEvalContext *pat_eval_ctx,
                                   const Move *move, const Rack *leave);
// The defense term of every non-placement move (exchange or pass): the
// position baseline, exactly, since they leave the board unchanged. Zero
// when the context is NULL or disabled.
static inline Equity
pat_eval_non_placement_penalty(const PATEvalContext *pat_eval_ctx) {
  if (!pat_eval_ctx || !pat_eval_ctx->weights) {
    return 0;
  }
  return pat_eval_ctx->pre_penalty + pat_eval_ctx->utility_non_placement;
}
// The largest utility correction (see PAT_UTILITY_ADJUST_ROW_PREFIX) any
// move can get at this position: every other PAT term is <= 0, so an
// upper bound on a move's equity that leaves the PAT term out, or bounds it
// by 0, stays a bound only once this is added. Zero when the context is
// NULL, disabled, or carries no correction.
static inline Equity
pat_eval_utility_bound(const PATEvalContext *pat_eval_ctx) {
  if (!pat_eval_ctx || !pat_eval_ctx->weights) {
    return 0;
  }
  return pat_eval_ctx->utility_bound;
}
// Bitmask of PAT_CLASS_MASK_* classes this context actually applies, or 0
// when the context is NULL or disabled. See placement_adjustment, which
// uses PAT_CLASS_MASK_WORD_MULT/PAT_CLASS_MASK_LETTER_MULT against this to
// skip whichever axis of the legacy opening penalty a live class already
// prices.
static inline uint32_t
pat_eval_context_get_active_classes(const PATEvalContext *pat_eval_ctx) {
  if (!pat_eval_ctx || !pat_eval_ctx->weights) {
    return 0;
  }
  return pat_eval_ctx->active_classes_mask;
}
// A penalty or bound as the context reports it: scaled by the position's
// stage factor. The factor is nonnegative, so a bound stays a bound and a
// penalty stays <= 0; rounding is monotone, so their order is kept too.
static inline Equity pat_eval_scaled(const PATEvalContext *pat_eval_ctx,
                                     Equity term) {
  if (pat_eval_ctx->term_scale == 1.0) {
    return term;
  }
  return (Equity)lround((double)term * pat_eval_ctx->term_scale);
}

// The upper bound on the defense term of every tile placement in lane
// `lane` of direction `dir` (see lane_penalty_bound). Zero when the context
// is NULL or disabled.
static inline Equity
pat_eval_lane_penalty_bound(const PATEvalContext *pat_eval_ctx, int dir,
                            int lane) {
  if (!pat_eval_ctx || !pat_eval_ctx->weights) {
    return 0;
  }
  return pat_eval_ctx->lane_penalty_bound[dir][lane] +
         pat_eval_ctx->utility_bound;
}

#endif
