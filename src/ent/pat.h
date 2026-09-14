#ifndef PAT_H
#define PAT_H

#include "../def/pat_defs.h"
#include "../util/io_util.h"
#include "board.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
#include <stddef.h>
#include <stdint.h>

// Trained PAT (Positional Adjustment Table) weights: a small vector of
// penalties applied to static move equity based on the opponent's post-move
// access to triple word squares. Every applied weight is <= 0. This sign
// convention is load-bearing:
// the defense term is deliberately omitted from the shadow equity upper bound
// (see static_eval_get_shadow_equity), which is only sound for terms that can
// never increase a move's equity. The loader and setters enforce it.
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
// The mutation counter changes whenever the weights are rewritten in place
// (as the training loop does between generations) so any future cache keyed
// on this object can detect staleness.
// See PATWeights.combine_gamma.
double pat_get_combine_gamma(const PATWeights *pat);
void pat_set_combine_gamma(PATWeights *pat, double combine_gamma);
// See PATWeights.own_asset_discount.
double pat_get_own_asset_discount(const PATWeights *pat);
void pat_set_own_asset_discount(PATWeights *pat, double own_asset_discount);
bool pat_get_lexicon_floaters(const PATWeights *pat);
void pat_set_lexicon_floaters(PATWeights *pat, bool lexicon_floaters);
bool pat_get_signed_through(const PATWeights *pat);
void pat_set_signed_through(PATWeights *pat, bool signed_through);
bool pat_get_fit_scaled_channels(const PATWeights *pat);
void pat_set_fit_scaled_channels(PATWeights *pat, bool fit_scaled_channels);
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
// Entries of the floater through-table; see PATWeights.through_score.
int pat_get_through_score(const PATWeights *pat, MachineLetter ml, int span);
// The end-specific through tables: word_end 0 when ml is the word's first
// letter, 1 when its last (see pat_walk_words).
int pat_get_through_score_end(const PATWeights *pat, int word_end,
                              MachineLetter ml, int span);
int pat_get_through_count_end(const PATWeights *pat, int word_end,
                              MachineLetter ml, int span);
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
  // The standard 15x15 board has 61 premium squares of the four classes
  // walked (8 triple word, 17 double word, 12 triple letter, 24 double
  // letter) and the 21x21 super board has 125. Truncation past the cap is
  // deterministic (row-major) but it is also a defect: the trainer lists
  // squares on the post-move board and the engine on the pre-move board,
  // so a covered square near the cap shifts which squares each side sees.
  // Keep the cap above every layout that is built.
  // The super board walks 125 premium squares (4 quad word, 16 triple
  // word, 41 double word, 8 quad letter, 20 triple letter, 36 double
  // letter) and has 72 windows.
  PAT_MAX_PREMIUM = 132,
  // The standard board has 16 double-double windows (8 in rows and 8 in
  // columns) and the super board 40. Windows are found horizontally first,
  // so a cap below the count silently drops every vertical window.
  PAT_MAX_DD = 80,
  // The unit masks are 64-bit, so this is the hard ceiling (see the
  // static_assert in pat.c).
  PAT_MAX_SCAN_UNITS = PAT_MAX_PREMIUM * 2 + PAT_MAX_DD,
  // The affected-unit set no longer fits one word.
  PAT_MASK_WORDS = (PAT_MAX_SCAN_UNITS + 63) / 64,
  // Double word squares further apart than this cannot be joined by one
  // word even with playthrough, so they are not a window.
  PAT_DD_MAX_SPAN = 2 * RACK_SIZE,
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
typedef struct PATEvalContext {
  // NULL means the context is disabled and the defense term is zero.
  const PATWeights *weights;
  const LetterDistribution *ld;
  const Square *lanes;
  Equity pre_penalty;
  // The premium squares walked, in row-major order within each class, and
  // which class each belongs to.
  int num_tws;
  uint8_t tws_rows[PAT_MAX_PREMIUM];
  uint8_t tws_cols[PAT_MAX_PREMIUM];
  uint8_t tws_classes[PAT_MAX_PREMIUM];
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
  // Bitmask of PAT_CLASS_MASK_* classes this context actually applies
  // (weighted in the file and not excluded by the runtime mask); see
  // pat_eval_ctx_active_classes, the safe way to read this from outside
  // pat.c since it is meaningless (and left unset) while weights is NULL.
  uint32_t active_classes_mask;
  // The opponent's rack SIZE only -- never their rack contents, which real
  // play never has visibility into. A route needing more fresh tiles than
  // the opponent currently holds cannot be played regardless of which
  // letters admit it, so scans cap distance at this rather than always at
  // RACK_SIZE. Defaults to RACK_SIZE when a caller has no better estimate
  // (e.g. training, which observes a specific player but not turn parity
  // detail beyond that).
  //
  // Every current caller is bag-gated (see pat_eval_context_load's own
  // callers in move_gen.c and validated_move.c, and the training gate in
  // autoplay.c), and the opponent's rack can only fall below RACK_SIZE once
  // the bag is empty: whichever player's draw first comes up short is the
  // one that empties it, so bag_get_letters(...) > 0 implies every rack
  // still on the board is full. This field is therefore always exactly
  // RACK_SIZE at every current call site -- confirmed empirically (zero
  // engagements over 4000 self-played games with instrumentation). It is
  // still correct and load-bearing as the plumbing pat_scan_unit's
  // hypergeometric reweighting (unseen_counts scaled by
  // opponent_rack_size / total_unseen, which DOES vary while the bag has
  // tiles) needs through the same call chain; keeping the cap live now
  // means it activates for free the day PAT's own bag-empty gate loosens.
  int opponent_rack_size;
  // Bit L of unit_hook_letters[u] is set when some live hook or floater
  // route this unit's baseline scan found would accept machine letter L
  // (blanks excluded, matching pat_set_flex's own convention). Lets a
  // move's own leave be checked against exactly the letters that would
  // let it exploit this unit itself, entirely independent of whether the
  // move's placement geometrically touches the unit -- unlike
  // unit_mask_by_row/col, which only ever answer the geometric question.
  // Computed once per position load, from the same overlay-free baseline
  // walk that fills unit_penalty; never updated for a per-move overlay
  // rescan.
  uint64_t unit_hook_letters[PAT_MAX_SCAN_UNITS];
  // Reverse index of the above: bit u of units_by_hook_letter[L] is set
  // exactly when unit_hook_letters[u] has bit L set. A move's leave has at
  // most RACK_SIZE distinct letters, so ORing this in for each one finds
  // every unit the move's own leave could exploit in a handful of array
  // reads, without rescanning a single unit the move's placement did not
  // already touch.
  uint64_t units_by_hook_letter[MAX_ALPHABET_SIZE][PAT_MASK_WORDS];
  // Units reachable through some letter the player's STARTING rack holds
  // right now (or every unit with any live route at all, if the rack has a
  // blank) -- a conservative, position-level superset of what any single
  // move's own leave could end up eligible for, since every leave is a
  // subset of this same rack. Empty whenever the file carries no discount.
  // Used both to widen lane_penalty_bound uniformly (a per-lane bound
  // computed before any specific move's leave exists) and as the fallback
  // pat_eval_move_penalty_bound takes when a caller has no specific leave
  // to offer: omitting leave information must never make a claimed upper
  // bound smaller than the true value could reach, so the fallback has to
  // be a safe superset, not empty.
  uint64_t worst_case_leave_units[PAT_MASK_WORDS];
} PATEvalContext;

void pat_eval_context_disable(PATEvalContext *pat_eval_ctx);
// Builds the context static evaluation uses. Premium squares whose class
// has no nonzero weight, or that enabled_classes_mask excludes, and
// windows whose tier has none or that the mask's PAT_CLASS_MASK_WINDOWS
// bit excludes, are not walked: their penalty would be exactly zero, so
// leaving them out changes no result and only saves the scans. Pass
// PAT_CLASS_MASK_ALL for ordinary use. opponent_rack_size is the tile
// COUNT only (public information), never rack contents; pass RACK_SIZE if
// unknown. Training never comes through here (it extracts every feature
// from the board directly), so a class the weights have not learned yet
// still reaches the fit.
void pat_eval_context_load(PATEvalContext *pat_eval_ctx,
                           const PATWeights *weights, const Square *lanes,
                           const LetterDistribution *ld,
                           const Rack *player_rack,
                           uint32_t enabled_classes_mask,
                           int opponent_rack_size);
// The same context with every unit walked whatever its weights, for callers
// that read per-move feature rows (pat_extract_move_features) and need the
// channels the current weights leave at zero.
void pat_eval_context_load_all_units(PATEvalContext *pat_eval_ctx,
                                     const PATWeights *weights,
                                     const Square *lanes,
                                     const LetterDistribution *ld,
                                     const Rack *player_rack,
                                     int opponent_rack_size);
// Returns the defense term for the move: the penalty for the opponent's TWS
// access after the move is played, which is always <= 0. Returns 0 when the
// context is NULL or disabled. Non-placement moves return the position
// baseline (their play leaves the board unchanged). leave is the tiles the
// move would keep, used only to credit units its own leave could exploit
// itself (see PATWeights.own_asset_discount and unit_hook_letters in the
// context above); pass NULL if unavailable, which simply forgoes the
// credit.
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
  return pat_eval_ctx->pre_penalty;
}
// Bitmask of PAT_CLASS_MASK_* classes this context actually applies, or 0
// when the context is NULL or disabled. See placement_adjustment, which
// uses PAT_CLASS_MASK_WORD_MULT/PAT_CLASS_MASK_LETTER_MULT against this to
// skip whichever axis of the legacy opening penalty a live class already
// prices.
static inline uint32_t
pat_eval_ctx_active_classes(const PATEvalContext *pat_eval_ctx) {
  if (!pat_eval_ctx || !pat_eval_ctx->weights) {
    return 0;
  }
  return pat_eval_ctx->active_classes_mask;
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
  return pat_eval_ctx->lane_penalty_bound[dir][lane];
}
// Extracts the feature vector for the board as it stands (no move overlay).
// Used for the context baseline and, exactly as-is, by the training loop on
// post-move boards. features must have PAT_NUM_FEATURES elements.
// pat supplies the per-letter tables the floater channels read and may be
// NULL, which zeroes those channels. opponent_rack_size is the tile COUNT
// only (public information); pass RACK_SIZE if unknown.
void pat_extract_features(const Square *lanes, const LetterDistribution *ld,
                          const Rack *player_rack, const PATWeights *pat,
                          int opponent_rack_size, int32_t *features);
// The feature row to regress on when per-unit penalties are combined with a
// gamma below one. Because the combination charges the worst unit in full
// and the rest at gamma, the row whose dot product with the weights equals
// the combined term is gamma times every unit plus the remaining (1 - gamma)
// of whichever unit the CURRENT weights rank worst. Training on this row and
// evaluating with the same combination is one round of the obvious
// fixed-point iteration, which the generation loop already provides. Falls
// back to the plain sum when the weights combine at gamma 1 or rank every
// unit equally. features has PAT_NUM_FEATURES elements.
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
