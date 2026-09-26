#ifndef PAT_H
#define PAT_H

#include "../def/board_defs.h"
#include "../def/pat_defs.h"
#include "../util/io_util.h"
#include "board.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
#include "win_pct.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>

// Trained PAT (Positional Adjustment Term) weights: penalties applied to static
// move equity for the opponent's post-move access to premium squares. Every
// weight is <= 0, which the loader and setters enforce: the defense term is
// left out of the shadow equity upper bound (see
// static_eval_get_shadow_equity), which is only sound for terms that never
// raise a move's equity.
typedef struct PATWeights PATWeights;

// Loads weights from data/strategy/<pat_name>.pat. Returns NULL and pushes to
// error_stack on failure.
PATWeights *pat_create(const char *data_paths, const char *pat_name,
                       ErrorStack *error_stack);
// Creates a weights object with every weight set to zero, which makes the
// defense term identically zero. Used to bootstrap training.
PATWeights *pat_create_zeroed(const char *pat_name);
void pat_destroy(PATWeights *pat);
const char *pat_get_name(const PATWeights *pat);
Equity pat_get_weight(const PATWeights *pat, int feature_index);
// weight must be <= 0.
void pat_set_weight(PATWeights *pat, int feature_index, Equity weight);
// See PATWeights.combine_gamma.
void pat_set_combine_gamma(PATWeights *pat, double combine_gamma);
// See PATWeights.own_asset_discount.
double pat_get_own_asset_discount(const PATWeights *pat);
void pat_set_own_asset_discount(PATWeights *pat, double own_asset_discount);
bool pat_get_lexicon_floaters(const PATWeights *pat);
void pat_set_lexicon_floaters(PATWeights *pat, bool lexicon_floaters);
bool pat_get_signed_through(const PATWeights *pat);
void pat_set_signed_through(PATWeights *pat, bool signed_through);
void pat_set_run_through(PATWeights *pat, bool run_through);
// tiles is 1..RACK_SIZE; values are milli-equity <= 0 (see
// PAT_OPENING_TILES_ROW_PREFIX).
Equity pat_get_opening_tiles_adjustment(const PATWeights *pat, int tiles);
void pat_set_opening_tiles_adjustment(PATWeights *pat, int tiles,
                                      Equity adjustment);
Equity pat_get_opening_exchange_adjustment(const PATWeights *pat);
void pat_set_opening_exchange_adjustment(PATWeights *pat, Equity adjustment);
// stage is a PAT_STAGE_* value; see PAT_STAGE_SCALE_EARLY_ROW_PREFIX.
double pat_get_stage_scale(const PATWeights *pat, int stage);
void pat_set_stage_scale(PATWeights *pat, int stage, double scale);
// The stage a pre-move bag count falls in.
static inline int pat_stage_for_bag(int bag_count) {
  if (bag_count >= PAT_STAGE_EARLY_MIN_BAG) {
    return PAT_STAGE_EARLY;
  }
  return (bag_count >= PAT_STAGE_MID_MIN_BAG) ? PAT_STAGE_MID : PAT_STAGE_LATE;
}
// The run-keyed through tables (see PATWeights.run_through): the
// log-scaled count and mean rest-of-word score of words of the given
// length whose first (word_end 0) or last (word_end 1) key_len letters,
// in word order, are key. Zero when the tables were never prepared.
int pat_get_run_through_count(const PATWeights *pat, int word_end,
                              const MachineLetter *key, int key_len,
                              int word_length);
int pat_get_run_through_score(const PATWeights *pat, int word_end,
                              const MachineLetter *key, int key_len,
                              int word_length);
void pat_set_exact_created_hooks(PATWeights *pat, bool exact_created_hooks);
// A PAT_FIT_* value; see PAT_FIT_RESIDUAL_ROW_PREFIX.
int pat_get_fit_residual(const PATWeights *pat);
void pat_set_fit_residual(PATWeights *pat, int fit_residual);
// Changes whenever the weights are rewritten in place (as training does
// between generations), so a cache keyed on the object can detect staleness.
uint64_t pat_get_mutation_counter(const PATWeights *pat);
void pat_bump_mutation_counter(PATWeights *pat);
// Writes the weights to data/strategy/<pat_name>.pat.
void pat_write(const PATWeights *pat, const char *data_paths,
               const char *pat_name, ErrorStack *error_stack);
// Writes the canonical name of a feature index into buf (e.g. "hook_d2",
// "float_flex_d1", "tt_floater").
void pat_feature_name(int feature_index, char *buf, size_t buf_size);
// Builds the per-letter flexibility table (the number of two-letter words
// containing each letter) from the lexicon. Used to approximate the hook
// and extension flexibility of squares whose real cross and extension sets
// do not exist yet because they are created by the move being evaluated.
// Must be called before the weights are used for evaluation; kwg may be
// NULL, which zeroes the table.
void pat_prepare_hook_flex(PATWeights *pat, const KWG *kwg,
                           const LetterDistribution *ld);
// Returns the flexibility table entry for an (unblanked) machine letter.
int pat_get_hook_flex(const PATWeights *pat, MachineLetter ml);
// The end-specific through tables: word_end 0 when ml is the word's first
// letter, 1 when its last (see pat_walk_words).
int pat_get_through_score_end(const PATWeights *pat, int word_end,
                              MachineLetter ml, int span);
int pat_get_through_count_end(const PATWeights *pat, int word_end,
                              MachineLetter ml, int span);
// Entries of the unsigned floater through-table; see
// PATWeights.through_count.
int pat_get_through_count(const PATWeights *pat, MachineLetter ml, int span);

// The premium squares a lane walk can be anchored on. Each is worth
// reaching for a different reason, so each keeps its own feature channels.
typedef enum {
  PAT_PREMIUM_TWS,
  PAT_PREMIUM_DWS,
  PAT_PREMIUM_TLS,
  PAT_PREMIUM_DLS,
  PAT_PREMIUM_QWS,
  PAT_PREMIUM_QLS,
  PAT_NUM_PREMIUM_CLASSES,
} pat_premium_class_t;

// A runtime mask of which loaded classes pat_eval_context_load actually
// applies, independent of what the weights file has: bit
// (1u << a pat_premium_class_t value) gates that premium class, and
// PAT_CLASS_MASK_WINDOWS separately gates the double-word and larger
// window channels (double-double on the standard board). This lets one
// loaded PATWeights serve a fast mode (e.g. TWS only, for rollouts) and a
// full mode (e.g. every class, for candidate selection) without loading
// different files: a class this mask excludes is treated exactly like one
// the file weighted at zero, so it costs nothing to walk (see
// pat_class_is_weighted) and cannot change a result.
enum {
  PAT_CLASS_MASK_WINDOWS = 1u << PAT_NUM_PREMIUM_CLASSES,
  PAT_CLASS_MASK_ALL =
      PAT_CLASS_MASK_WINDOWS | ((1u << PAT_NUM_PREMIUM_CLASSES) - 1),
  PAT_CLASS_MASK_TWS_ONLY = 1u << PAT_PREMIUM_TWS,
  // The classes sim rollouts use unless -patrolloutclasses says otherwise:
  // triple word (and on 21x21 quad word) squares plus windows, which keep
  // most of full PAT's value at a fraction of its cost.
  PAT_CLASS_MASK_ROLLOUT_DEFAULT =
      (1u << PAT_PREMIUM_TWS) | PAT_CLASS_MASK_WINDOWS |
      (BOARD_DIM >= 21 ? (1u << PAT_PREMIUM_QWS) : 0),
  // Which square-multiplier axis each premium class draws its
  // opening_move_word_penalties/opening_move_letter_penalties contribution
  // from (see placement_adjustment and board.h's update_opening_penalty):
  // word squares triple or double the whole word, letter squares multiply
  // one tile. Used to skip whichever axis a live PAT class already prices,
  // so the legacy per-square opening penalty and a trained PAT weight for
  // the same square never both apply.
  PAT_CLASS_MASK_WORD_MULT = (1u << PAT_PREMIUM_TWS) | (1u << PAT_PREMIUM_DWS) |
                             (1u << PAT_PREMIUM_QWS),
  PAT_CLASS_MASK_LETTER_MULT = (1u << PAT_PREMIUM_TLS) |
                               (1u << PAT_PREMIUM_DLS) |
                               (1u << PAT_PREMIUM_QLS),
};

enum {
  // The standard board has 61 premium squares and the 21x21 board 125. Keep
  // the cap above every layout: the trainer lists squares on the post-move
  // board and the engine on the pre-move board, so truncation near the cap
  // would shift which squares each side sees.
  PAT_MAX_PREMIUM = 132,
  // The standard board has 16 double-double windows (8 in rows and 8 in
  // columns) and the super board 40. Windows are found horizontally first,
  // so a cap below the count silently drops every vertical window.
  PAT_MAX_DD = 80,
  // The unit masks are 64-bit, so this is the hard ceiling (see the
  // static_assert in pat.c).
  PAT_MAX_SCAN_UNITS = PAT_MAX_PREMIUM * 2 + PAT_MAX_DD,
  PAT_MASK_WORDS = (PAT_MAX_SCAN_UNITS + 63) / 64,
  // Double word squares further apart than this cannot be joined by one
  // word even with playthrough, so they are not a window.
  PAT_DD_MAX_SPAN = 2 * RACK_SIZE,
};

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
  int32_t unit_features[PAT_MAX_SCAN_UNITS][PAT_NUM_FEATURES];
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
  // The features carrying a nonzero weight, so a unit's dot product visits
  // only those; every other term is exactly zero.
  int nonzero_feature_index[PAT_NUM_FEATURES];
  int num_nonzero_features;
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
  // pat.c since it is meaningless (and left unset) while weights is NULL.
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

// Parses a comma-separated list of class names (tws, dws, tls, dls, qws,
// qls, windows), or "all" or "none", into the PAT_CLASS_MASK_* bitmask of
// the classes named. Pushes ERROR_STATUS_PAT_INVALID_CLASSES_ARG and
// returns 0 on an unrecognized name.
uint32_t pat_parse_classes_mask(const char *value, ErrorStack *error_stack);
// The inverse of pat_parse_classes_mask: "all", "none", or a
// comma-separated list of the classes enabled_classes_mask contains, in
// canonical order. Caller owns the returned string.
char *pat_classes_mask_to_string(uint32_t enabled_classes_mask);

#endif
