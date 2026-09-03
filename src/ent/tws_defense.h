#ifndef TWS_DEFENSE_H
#define TWS_DEFENSE_H

#include "../def/tws_defense_defs.h"
#include "../util/io_util.h"
#include "board.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
#include <stddef.h>
#include <stdint.h>

// Trained TWS defense weights ("TWD"): a small vector of penalties applied to
// static move equity based on the opponent's post-move access to triple word
// squares. Every applied weight is <= 0. This sign convention is load-bearing:
// the defense term is deliberately omitted from the shadow equity upper bound
// (see static_eval_get_shadow_equity), which is only sound for terms that can
// never increase a move's equity. The loader and setters enforce it.
typedef struct TWDWeights TWDWeights;

// Loads weights from data/strategy/<twd_name>.twd. Returns NULL and pushes to
// error_stack on failure.
TWDWeights *twd_create(const char *data_paths, const char *twd_name,
                       ErrorStack *error_stack);
// Creates a weights object with every weight set to zero, which makes the
// defense term identically zero. Used to bootstrap training.
TWDWeights *twd_create_zeroed(const char *twd_name);
void twd_destroy(TWDWeights *twd);
const char *twd_get_name(const TWDWeights *twd);
Equity twd_get_weight(const TWDWeights *twd, int feature_index);
// weight must be <= 0.
void twd_set_weight(TWDWeights *twd, int feature_index, Equity weight);
// The mutation counter changes whenever the weights are rewritten in place
// (as the training loop does between generations) so any future cache keyed
// on this object can detect staleness.
// See TWDWeights.combine_gamma.
double twd_get_combine_gamma(const TWDWeights *twd);
void twd_set_combine_gamma(TWDWeights *twd, double combine_gamma);
uint64_t twd_get_mutation_counter(const TWDWeights *twd);
void twd_bump_mutation_counter(TWDWeights *twd);
// Writes the weights to data/strategy/<twd_name>.twd.
void twd_write(const TWDWeights *twd, const char *data_paths,
               const char *twd_name, ErrorStack *error_stack);
// Writes the canonical name of a feature index into buf (e.g. "hook_d2",
// "float_flex_d1", "tt_floater").
void twd_feature_name(int feature_index, char *buf, size_t buf_size);
// Builds the per-letter flexibility table (the number of two-letter words
// containing each letter) from the lexicon. Used to approximate the hook
// and extension flexibility of squares whose real cross and extension sets
// do not exist yet because they are created by the move being evaluated.
// Must be called before the weights are used for evaluation; kwg may be
// NULL, which zeroes the table.
void twd_prepare_hook_flex(TWDWeights *twd, const KWG *kwg,
                           const LetterDistribution *ld);
// Returns the flexibility table entry for an (unblanked) machine letter.
int twd_get_hook_flex(const TWDWeights *twd, MachineLetter ml);
// Entries of the floater through-table; see TWDWeights.through_score.
int twd_get_through_score(const TWDWeights *twd, MachineLetter ml, int span);
int twd_get_through_count(const TWDWeights *twd, MachineLetter ml, int span);

// The premium squares a lane walk can be anchored on. Each is worth
// reaching for a different reason, so each keeps its own feature channels.
typedef enum {
  TWD_PREMIUM_TWS,
  TWD_PREMIUM_DWS,
  TWD_PREMIUM_TLS,
  TWD_PREMIUM_QWS,
  TWD_PREMIUM_QLS,
  TWD_NUM_PREMIUM_CLASSES,
} twd_premium_class_t;

enum {
  // The standard 15x15 board has 37 premium squares of the three classes
  // walked (8 triple word, 17 double word, 12 triple letter) and the 21x21
  // super board has 77. Truncation past the cap is deterministic
  // (row-major) but it is also a defect: the trainer lists squares on the
  // post-move board and the engine on the pre-move board, so a covered
  // square near the cap shifts which squares each side sees. Keep the cap
  // above every layout that is built.
  TWD_MAX_TWS = 24,
  // The super board walks 89 premium squares (4 quad word, 16 triple word,
  // 41 double word, 8 quad letter, 20 triple letter) and has 72 windows.
  TWD_MAX_PREMIUM = 96,
  // The standard board has 16 double-double windows (8 in rows and 8 in
  // columns) and the super board 40. Windows are found horizontally first,
  // so a cap below the count silently drops every vertical window.
  TWD_MAX_DD = 80,
  // The unit masks are 64-bit, so this is the hard ceiling (see the
  // static_assert in tws_defense.c).
  TWD_MAX_SCAN_UNITS = TWD_MAX_PREMIUM * 2 + TWD_MAX_DD,
  // The affected-unit set no longer fits one word.
  TWD_MASK_WORDS = (TWD_MAX_SCAN_UNITS + 63) / 64,
  // Double word squares further apart than this cannot be joined by one
  // word even with playthrough, so they are not a window.
  TWD_DD_MAX_SPAN = 2 * RACK_SIZE,
};

// Per-position evaluation state, rebuilt by each movegen position load (and
// on the stack for validated moves). Holds the position-constant part of the
// defense term (pre_penalty, the penalty for the opponent's TWS access on
// the board as it stands) plus what the per-move delta needs to stay off
// the hot path: per scan unit (one TWS row or column walk), the baseline
// feature vector and the extent of squares the walk actually visited, so a
// candidate move rescans a unit only when it places a tile on or directly
// beside a square that walk could see, and rescans it exactly once (the
// baseline side is cached).
//
// The lanes pointer is only valid while the board is alive, untransposed,
// and unmutated, which holds for the duration of a move generation call and
// for a stack-scoped validated-move evaluation.
typedef struct TWDEvalContext {
  // NULL means the context is disabled and the defense term is zero.
  const TWDWeights *weights;
  const LetterDistribution *ld;
  const Square *lanes;
  Equity pre_penalty;
  // The premium squares walked, in row-major order within each class, and
  // which class each belongs to.
  int num_tws;
  uint8_t tws_rows[TWD_MAX_PREMIUM];
  uint8_t tws_cols[TWD_MAX_PREMIUM];
  uint8_t tws_classes[TWD_MAX_PREMIUM];
  // Double-double windows: window i runs along lane dd_lanes[i] of
  // direction dd_dirs[i], from lane square dd_los[i] to dd_his[i], whose
  // squares are both double word squares.
  int num_dd;
  uint8_t dd_dirs[TWD_MAX_DD];
  uint8_t dd_lanes[TWD_MAX_DD];
  uint8_t dd_los[TWD_MAX_DD];
  uint8_t dd_his[TWD_MAX_DD];
  // Which product tier each window's two multipliers put it in.
  uint8_t dd_tiers[TWD_MAX_DD];
  // Units 2*i and 2*i+1 are TWS i's horizontal and vertical walks; the
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
  // The evaluating player's own tiles. No feature reads this: it exists so
  // twd_extract_move_features can report the rack-relative diagnostics that
  // are deliberately not modelled (see TWDMoveDiagnostics).
  uint8_t own_counts[MAX_ALPHABET_SIZE];
  int32_t unit_features[TWD_MAX_SCAN_UNITS][TWD_NUM_FEATURES];
  // Each unit's baseline contribution to pre_penalty (always <= 0), used
  // to bound a move's penalty from above without rescanning.
  Equity unit_penalty[TWD_MAX_SCAN_UNITS];
  // Sum of every unit's baseline penalty, and the units ordered from the
  // most negative baseline up. A move's combination is then formed from
  // the units it reaches alone: the unreached sum is the total less the
  // reached baselines, and the unreached worst is the first unit in this
  // order the move does not reach.
  int64_t total_unit_penalty;
  uint16_t units_by_penalty[TWD_MAX_SCAN_UNITS];
  // The features carrying a nonzero weight, so a unit's dot product visits
  // only those; every other term is exactly zero.
  int nonzero_feature_index[TWD_NUM_FEATURES];
  int num_nonzero_features;
  // Bit u of unit_mask_by_row[r] is set when a fresh tile in row r could
  // affect unit u provided the move's column span also overlaps the unit
  // (and symmetrically for columns), so a candidate move's affected-unit
  // set is the AND of the OR of its rows' masks with the OR of its
  // columns' masks: a few operations per candidate on the movegen record
  // path, zero for the vast majority of moves.
  uint64_t unit_mask_by_row[BOARD_DIM][TWD_MASK_WORDS];
  uint64_t unit_mask_by_col[BOARD_DIM][TWD_MASK_WORDS];
  // lane_penalty_bound[dir][lane] is an upper bound on the defense term of
  // every tile placement along that lane (a row for horizontal moves, a
  // column for vertical ones): the baseline penalty less the baseline
  // contribution of every unit a move in the lane could affect, each of
  // which it can at best zero out. Shadow pruning adds it to its per-anchor
  // equity bounds, which otherwise omit the term entirely; it is always
  // <= 0, so the bounds stay valid, and it is a plain array lookup so the
  // shadow hot path pays nothing for it.
  Equity lane_penalty_bound[2][BOARD_DIM];
} TWDEvalContext;

void twd_eval_context_disable(TWDEvalContext *twd_eval_ctx);
// Builds the context static evaluation uses. Premium squares whose class
// has no nonzero weight, and windows whose tier has none, are not walked:
// their penalty would be exactly zero, so leaving them out changes no
// result and only saves the scans. Training never comes through here (it
// extracts every feature from the board directly), so a class the weights
// have not learned yet still reaches the fit.
void twd_eval_context_load(TWDEvalContext *twd_eval_ctx,
                           const TWDWeights *weights, const Square *lanes,
                           const LetterDistribution *ld,
                           const Rack *player_rack);
// The same context with every unit walked whatever its weights, for callers
// that read per-move feature rows (twd_extract_move_features) and need the
// channels the current weights leave at zero.
void twd_eval_context_load_all_units(TWDEvalContext *twd_eval_ctx,
                                     const TWDWeights *weights,
                                     const Square *lanes,
                                     const LetterDistribution *ld,
                                     const Rack *player_rack);
// Returns the defense term for the move: the penalty for the opponent's TWS
// access after the move is played, which is always <= 0. Returns 0 when the
// context is NULL or disabled. Non-placement moves return the position
// baseline (their play leaves the board unchanged).
Equity twd_eval_move_penalty(const TWDEvalContext *twd_eval_ctx,
                             const Move *move);
// Returns an upper bound on twd_eval_move_penalty for the move without any
// lane rescans: the baseline penalty minus the baseline contributions of
// the units the move can affect (each of which the move can at best zero
// out). Used to skip the exact computation for moves that cannot contend.
Equity twd_eval_move_penalty_bound(const TWDEvalContext *twd_eval_ctx,
                                   const Move *move);
// The defense term of every non-placement move (exchange or pass): the
// position baseline, exactly, since they leave the board unchanged. Zero
// when the context is NULL or disabled.
static inline Equity
twd_eval_non_placement_penalty(const TWDEvalContext *twd_eval_ctx) {
  if (!twd_eval_ctx || !twd_eval_ctx->weights) {
    return 0;
  }
  return twd_eval_ctx->pre_penalty;
}
// The upper bound on the defense term of every tile placement in lane
// `lane` of direction `dir` (see lane_penalty_bound). Zero when the context
// is NULL or disabled.
static inline Equity
twd_eval_lane_penalty_bound(const TWDEvalContext *twd_eval_ctx, int dir,
                            int lane) {
  if (!twd_eval_ctx || !twd_eval_ctx->weights) {
    return 0;
  }
  return twd_eval_ctx->lane_penalty_bound[dir][lane];
}
// Quantities that describe a candidate move but are deliberately kept out
// of the model. Contested and uncontested hook access split the same
// squares hook_dN counts by whether the evaluating player holds a tile that
// fits; own_monopoly counts the tiles they hold for hooks the opponent has
// no tile for at all. All three are functions of the player's own rack,
// which is identical across the candidate moves of one position, so fitting
// them against an absolute label learns rack strength rather than hook
// value (measured: doing so loses badly). They are computed only when asked
// for, to be fitted against a within-position label offline.
typedef struct TWDMoveDiagnostics {
  int32_t hook_contested;
  int32_t hook_uncontested;
  int32_t own_monopoly;
} TWDMoveDiagnostics;

// The feature row a candidate move produces, combined exactly as the term
// applies it (see TWDWeights.combine_gamma), so it is the row a fit should
// regress a label on. features has TWD_NUM_FEATURES elements. diagnostics
// may be NULL; when given it is filled from the same scans.
void twd_extract_move_features(const TWDEvalContext *twd_eval_ctx,
                               const Move *move, double *features,
                               TWDMoveDiagnostics *diagnostics);

// Extracts the feature vector for the board as it stands (no move overlay).
// Used for the context baseline and, exactly as-is, by the training loop on
// post-move boards. features must have TWD_NUM_FEATURES elements.
// twd supplies the per-letter tables the floater channels read and may be
// NULL, which zeroes those channels.
void twd_extract_features(const Square *lanes, const LetterDistribution *ld,
                          const Rack *player_rack, const TWDWeights *twd,
                          int32_t *features);
// The feature row to regress on when per-unit penalties are combined with a
// gamma below one. Because the combination charges the worst unit in full
// and the rest at gamma, the row whose dot product with the weights equals
// the combined term is gamma times every unit plus the remaining (1 - gamma)
// of whichever unit the CURRENT weights rank worst. Training on this row and
// evaluating with the same combination is one round of the obvious
// fixed-point iteration, which the generation loop already provides. Falls
// back to the plain sum when the weights combine at gamma 1 or rank every
// unit equally. features has TWD_NUM_FEATURES elements.
void twd_extract_features_combined(const Square *lanes,
                                   const LetterDistribution *ld,
                                   const Rack *player_rack,
                                   const TWDWeights *twd, double *features);

#endif
