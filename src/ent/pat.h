#ifndef PAT_H
#define PAT_H

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/pat_defs.h"
#include "../def/rack_defs.h"
#include "../util/io_util.h"
#include "equity.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Trained PAT (Positional Adjustment Term) weights: penalties applied to static
// move equity for the opponent's post-move access to premium squares. Every
// weight is <= 0, which the loader and setters enforce: the defense term is
// left out of the shadow equity upper bound (see
// static_eval_get_shadow_equity), which is only sound for terms that never
// raise a move's equity.
typedef struct PATWeights PATWeights;

struct PATWeights {
  char *name;
  Equity weights[PAT_NUM_FEATURES];
  // Per-letter count of two-letter words containing the letter; the
  // flexibility approximation for hooks and floaters the evaluated move
  // itself creates (see pat_prepare_hook_flex).
  uint8_t hook_flex[MAX_ALPHABET_SIZE];
  // What a floater is worth to whoever plays through it, from the lexicon.
  // through_score[ml][len] is the mean tile value the rest of a len-letter
  // word carries when ml sits at one end of it, and through_count[ml][len]
  // is how many such words there are, log-scaled. Indexed by the span a word
  // must cover to run from the floater to the premium.
  uint8_t through_score[MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  uint8_t through_count[MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  // The same statistics kept separately for the letter being the word's first
  // letter ([0]) or its last ([1]): a floater beyond the premium ends the
  // word that reaches it, a floater before it starts it, and letters like J
  // and Y differ enormously between the two. Used only when signed_through
  // is set.
  uint8_t through_score_end[2][MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  uint8_t through_count_end[2][MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  // Run-keyed through tables: for a key of 1..PAT_RUN_THROUGH_MAX_KEY
  // letters, per word length and per end, the same two statistics over
  // the words that begin (end 0) or end (end 1) with that key. Built by
  // pat_prepare_hook_flex, sized by the alphabet; see pat_run_through_index.
  uint8_t *run_through_count;
  uint8_t *run_through_score;
  int run_through_alphabet_size;
  // How much a second route to danger counts once the worst is counted. The
  // opponent plays one move, so threats do not simply add: 0 charges only
  // the worst scan unit, 1 charges every unit in full, and values between
  // allow for a rack that cannot use the worst route. See
  // pat_combine_unit_penalties.
  double combine_gamma;
  // Fraction of a unit's penalty credited back when the move's own leave
  // holds a letter that could exploit that unit, whether or not the move
  // touches it; 0 means no credit. Clamped to [0, 1] so the adjusted penalty
  // p * (1 - discount) stays in [p, 0] and the shadow bounds stay valid.
  double own_asset_discount;
  // Whether a floater run's flexibility counts only the unseen tiles the
  // lexicon lets extend the run toward the premium (its real extension set)
  // or every unseen tile. The two are different models, so weights fitted
  // under one must be evaluated under the same one; hence a per-file flag.
  bool lexicon_floaters;
  // Whether pat_scan_unit reads the end-specific through tables for a
  // floater, by which side of the premium it lies on, instead of the
  // unsigned tables. Per file, like lexicon_floaters.
  bool signed_through;
  // Which channels patgen fits, the rest keeping their loaded weights
  // (PAT_FIT_*; see PAT_FIT_RESIDUAL_ROW_PREFIX), so a refit of a few
  // channels is not swamped by the noise of a whole-vector refit.
  int fit_residual;
  // Whether hooks the evaluated move creates are scored exactly (see
  // pat_fresh_cross_set) rather than from the hook_flex approximation. Per
  // file, like the floater flags.
  bool exact_created_hooks;
  // Whether a floater run holding a tile the evaluated move places is scored
  // from the run's real extension set toward the premium (see
  // pat_fresh_run_extension_set) rather than from the hook_flex of the tile
  // facing the premium, which counts two-letter words across the lane
  // instead of extensions along it. Per file, like the floater flags.
  bool exact_fresh_runs;
  // Whether hook_score channels measure a hook by the chance the opponent
  // can fill it times the best fill's score (see pat_hook_score_exposure)
  // rather than by the sum over every unseen tile that fits. The sum grows
  // with each fitting tile though the opponent plays one, so a linear fit
  // over it underprices a hook few tiles fill. Per file, like the floater
  // flags.
  bool hook_score_prob;
  // Whether floater through channels use the run-keyed tables (see
  // pat_scan_unit). The per-tile sum they replace adds each tile's
  // single-letter log-count, which badly overstates runs like QI that few or
  // no words contain.
  bool run_through;
  // See PAT_UTILITY_ADJUST_ROW_PREFIX. The tables are built from a win
  // percentage table by pat_prepare_utility (or pat_set_utility_adjust)
  // when utility_adjust is nonzero and are NULL otherwise, indexed
  // [bag * PAT_UTILITY_WIDTH + margin + PAT_UTILITY_MARGIN_LIMIT] by the
  // bag after the move: the correction itself, and its maximum over all
  // margins at or above the index (a move can only add score).
  // utility_win_pct_name (owned) names the table they came from.
  double utility_adjust;
  int utility_max_bag;
  Equity *utility_table;
  Equity *utility_suffix_max;
  char *utility_win_pct_name;
  // Per-stage factor on the applied term (see the row prefixes in
  // pat_defs.h); indexed by PAT_STAGE_*.
  double stage_scale[PAT_STAGE_COUNT];
  // Opening adjustments by tiles played (index 1..RACK_SIZE) and for an
  // opening exchange, milli-equity <= 0 (see PAT_OPENING_TILES_ROW_PREFIX).
  Equity opening_tiles[RACK_SIZE + 1];
  Equity opening_exchange;
  // Set by pat_prepare_hook_flex. The lexicon tables are part of the model,
  // so evaluation and training refuse an unprepared one rather than scoring
  // with empty tables.
  bool prepared;
  uint64_t mutation_counter;
  // The version named on the file's header line (see PAT_VERSION).
  int version;
};

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
void pat_set_exact_created_hooks(PATWeights *pat, bool exact_created_hooks);
void pat_set_exact_fresh_runs(PATWeights *pat, bool exact_fresh_runs);
void pat_set_hook_score_prob(PATWeights *pat, bool hook_score_prob);
// A PAT_FIT_* value; see PAT_FIT_RESIDUAL_ROW_PREFIX.
int pat_get_fit_residual(const PATWeights *pat);
void pat_set_fit_residual(PATWeights *pat, int fit_residual);
// Changes whenever the weights are rewritten in place (as training does
// between generations), so a cache keyed on the object can detect staleness.
uint64_t pat_get_mutation_counter(const PATWeights *pat);
void pat_bump_mutation_counter(PATWeights *pat);
// Aborts when pat's lexicon tables were never prepared (see
// PATWeights.prepared). NULL passes.
void pat_require_prepared(const PATWeights *pat);
// Writes the canonical name of a feature index into buf (e.g. "hook_d2",
// "float_flex_d1", "tt_floater").
void pat_feature_name(int feature_index, char *buf, size_t buf_size);

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
  // static_assert in pat_eval.c).
  PAT_MAX_SCAN_UNITS = PAT_MAX_PREMIUM * 2 + PAT_MAX_DD,
  PAT_MASK_WORDS = (PAT_MAX_SCAN_UNITS + 63) / 64,
  // Double word squares further apart than this cannot be joined by one
  // word even with playthrough, so they are not a window.
  PAT_DD_MAX_SPAN = 2 * RACK_SIZE,
};

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
