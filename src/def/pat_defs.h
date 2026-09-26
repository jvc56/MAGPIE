#ifndef PAT_DEFS_H
#define PAT_DEFS_H

#include "rack_defs.h"

// Feature layout for the PAT weights (see src/ent/pat.h).
//
// Each feature measures one channel of the opponent's access to an empty
// premium square, binned by the number of empty squares a word reaching the
// square must fill ("d"), which is the number of tiles the opponent must
// play. Both channels start at d = 1: a hook at d = 1 is a hookable premium
// square itself (a one-tile play), and a floater at d = 1 is a playthrough
// tile directly adjacent to the square. A file with no row for a feature
// reads it as zero.
enum {
  PAT_HOOK_BIN_COUNT = RACK_SIZE,
  PAT_FLOATER_BIN_COUNT = RACK_SIZE,
  PAT_FEATURE_HOOK_START = 0,
  PAT_FEATURE_FLOAT_FLEX_START = PAT_FEATURE_HOOK_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_FLOAT_SCORE_START =
      PAT_FEATURE_FLOAT_FLEX_START + PAT_FLOATER_BIN_COUNT,
  // A floater's face value does not say what it is worth to the opponent;
  // the words that reach the premium through it do. These channels read a
  // table built from the lexicon, keyed by the floater's letter and the span
  // the word must cover: the mean tile value such a word lays down besides
  // the floater, and how many such words exist.
  PAT_FEATURE_FLOAT_THROUGH_SCORE_START =
      PAT_FEATURE_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_FLOAT_THROUGH_COUNT_START =
      PAT_FEATURE_FLOAT_THROUGH_SCORE_START + PAT_FLOATER_BIN_COUNT,
  // The hook and floater-flexibility sums scaled by the hypergeometric
  // expectation opponent_rack_size / total_unseen, since most unseen tiles
  // are in the bag rather than the opponent's rack while the bag is large.
  // Triple word squares only.
  PAT_FEATURE_HOOK_SCALED_START =
      PAT_FEATURE_FLOAT_THROUGH_COUNT_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_FLOAT_FLEX_SCALED_START =
      PAT_FEATURE_HOOK_SCALED_START + PAT_HOOK_BIN_COUNT,
  // Hook flexibility weighted by each admissible letter's immediate score:
  // the hooked word's score under the hook square's word multiplier plus the
  // letter's value under its letter multiplier, counted in the hook word and
  // in the lane word the premium multiplies (see pat_effective_cross_info),
  // divided by PAT_HOOK_SCORE_SCALE. Triple word squares only.
  PAT_FEATURE_HOOK_SCORE_START =
      PAT_FEATURE_FLOAT_FLEX_SCALED_START + PAT_FLOATER_BIN_COUNT,
  // Hook and floater-value channels for each other premium class, so the fit
  // says what each class is worth.
  PAT_FEATURE_DWS_HOOK_START =
      PAT_FEATURE_HOOK_SCORE_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_DWS_FLOAT_SCORE_START =
      PAT_FEATURE_DWS_HOOK_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_TLS_HOOK_START =
      PAT_FEATURE_DWS_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_TLS_FLOAT_SCORE_START =
      PAT_FEATURE_TLS_HOOK_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_DLS_HOOK_START =
      PAT_FEATURE_TLS_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_DLS_FLOAT_SCORE_START =
      PAT_FEATURE_DLS_HOOK_START + PAT_HOOK_BIN_COUNT,
  // The 21x21 board's quadruple word and quadruple letter squares.
  PAT_FEATURE_QWS_HOOK_START =
      PAT_FEATURE_DLS_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_QWS_FLOAT_SCORE_START =
      PAT_FEATURE_QWS_HOOK_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_QLS_HOOK_START =
      PAT_FEATURE_QWS_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_QLS_FLOAT_SCORE_START =
      PAT_FEATURE_QLS_HOOK_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_TT_FLOATER =
      PAT_FEATURE_QLS_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_TT_HOOK_ONLY = PAT_FEATURE_TT_FLOATER + 1,
  // Windows: two empty word-multiplier squares in one lane close enough for
  // a single word to cover both, binned by the product of the multipliers
  // (only double-doubles on the standard board; the 21x21 board adds
  // double-triple, triple-triple and triple-quad). A window counts as floater
  // access when a playthrough tile sits inside it and as hook-only access
  // when it is empty but has a hookable square; tiles saved is RACK_SIZE
  // minus the empty squares the window still needs.
  PAT_WINDOW_TIER_COUNT = 4,
  PAT_WINDOW_FEATURES_PER_TIER = 3,
  PAT_FEATURE_WINDOW_START = PAT_FEATURE_TT_HOOK_ONLY + 1,
  // Tier 0 is the double-double.
  PAT_FEATURE_DD_FLOATER = PAT_FEATURE_WINDOW_START,
  PAT_FEATURE_DD_HOOK_ONLY = PAT_FEATURE_DD_FLOATER + 1,
  PAT_FEATURE_DD_TILES_SAVED = PAT_FEATURE_DD_HOOK_ONLY + 1,
  // Letter multipliers along a reaching word: per route, the largest letter
  // multiplier among the empty squares the word must cover adds word
  // multiplier x (letter multiplier - 1), binned by the route's distance
  // ("span"). A letter multiplier reachable only by extending past the
  // contact or the premium, within the rack, is counted separately ("ext")
  // when it beats the span's. Routes are counted only when some unseen tile
  // fits. Triple and double word lanes have separate channels.
  PAT_FEATURE_LM_SPAN_START =
      PAT_FEATURE_WINDOW_START +
      PAT_WINDOW_TIER_COUNT * PAT_WINDOW_FEATURES_PER_TIER,
  PAT_FEATURE_LM_EXT_START = PAT_FEATURE_LM_SPAN_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_DWS_LM_SPAN_START = PAT_FEATURE_LM_EXT_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_DWS_LM_EXT_START =
      PAT_FEATURE_DWS_LM_SPAN_START + PAT_HOOK_BIN_COUNT,
  PAT_NUM_FEATURES = PAT_FEATURE_DWS_LM_EXT_START + PAT_HOOK_BIN_COUNT,
};

// A training observation is labeled with the opponent's net gain over the
// next this-many plies, so at most this many wait to be labeled at once.
#define PAT_MAX_LABEL_PLIES 6

// The through-table spans word lengths 2 up to this; a floater further from
// the premium cannot be reached by one word anyway.
#define PAT_MAX_THROUGH_LEN 16

// Optional row: how per-unit penalties combine (see
// PATWeights.combine_gamma). Absent means 1.0, the plain sum.
#define PAT_GAMMA_ROW_PREFIX "gamma,"
#define PAT_DEFAULT_COMBINE_GAMMA 1.0
// What training uses unless told otherwise: a second route to danger is
// worth half a first.
#define PAT_TRAINING_COMBINE_GAMMA 0.5

// Optional row: the fraction of a unit's penalty credited back when the
// move's own leave could exploit that unit (see
// PATWeights.own_asset_discount). Absent means 0, no credit.
#define PAT_OWN_ASSET_DISCOUNT_ROW_PREFIX "own_asset_discount,"
#define PAT_DEFAULT_OWN_ASSET_DISCOUNT 0.0

// Optional row (0 or 1): whether a floater run's flexibility counts only the
// unseen tiles the lexicon lets extend it (see PATWeights.lexicon_floaters).
// Absent means 0, every unseen tile.
#define PAT_LEXICON_FLOATERS_ROW_PREFIX "lexicon_floaters,"
#define PAT_DEFAULT_LEXICON_FLOATERS false

// Optional row (0 or 1): whether the floater through-tables distinguish
// which end of the opponent's word the floater would be (see
// PATWeights.signed_through). Absent means 0.
#define PAT_SIGNED_THROUGH_ROW_PREFIX "signed_through,"
#define PAT_DEFAULT_SIGNED_THROUGH false

// Optional row (0 or 1): whether patgen fits the hypergeometric-scaled
// channels or leaves them at zero (see PATWeights.fit_scaled_channels).
// Absent means 1.
#define PAT_FIT_SCALED_ROW_PREFIX "fit_scaled,"
#define PAT_DEFAULT_FIT_SCALED true

// Optional row (0 or 1): whether patgen builds training rows through the
// runtime overlay path instead of scanning the post-move board (see
// PATWeights.train_overlay). Absent means 0.
#define PAT_TRAIN_OVERLAY_ROW_PREFIX "train_overlay,"
#define PAT_DEFAULT_TRAIN_OVERLAY false

// Optional row (0 to 5): which channels patgen fits, the rest keeping their
// loaded values (see PATWeights.fit_residual). 1: the hook-score channels
// (PAT_FEATURE_HOOK_SCORE_START onward, PAT_HOOK_BIN_COUNT of them); 2:
// those and the triple word hook flexibility channels; 3: the floater
// through channels; 4: the letter-multiplier channels
// (PAT_FEATURE_LM_SPAN_START onward); 5: every channel, hook-score
// included. Absent means 0: every channel but hook-score.
#define PAT_FIT_RESIDUAL_ROW_PREFIX "fit_residual,"
#define PAT_DEFAULT_FIT_RESIDUAL false

// Optional row (0 or 1): whether a hook the evaluated move creates is scored
// from its real cross set, resolved on the GADDAG, instead of the two-letter
// word approximation (see PATWeights.exact_created_hooks). Absent means 0.
// Needs the context's KWG (pat_eval_context_set_kwg).
#define PAT_EXACT_CREATED_HOOKS_ROW_PREFIX "exact_created_hooks,"
#define PAT_DEFAULT_EXACT_CREATED_HOOKS false

// Optional row (0 or 1): whether the floater through channels score a run of
// tiles by the words containing the whole run at the required end, up to
// PAT_RUN_THROUGH_MAX_KEY letters, instead of summing each tile's
// single-letter statistic (see PATWeights.run_through). Absent means 0.
#define PAT_RUN_THROUGH_ROW_PREFIX "run_through,"
#define PAT_DEFAULT_RUN_THROUGH false
// Runs longer than this are keyed by their far-end letters, whose word count
// is a superset of the run's.
#define PAT_RUN_THROUGH_MAX_KEY 3

// Optional row (0 or 1): whether patgen, instead of refitting, writes one
// candidate file per shrinkage strength, each pulling every free coefficient
// toward its loaded value, with its held-out reply-prediction error. The
// output file keeps the loaded weights, since prediction error does not
// select playing strength; whole-game validation does. One game pair in
// PAT_GEN_HELDOUT_EVERY is held out. Absent means 0.
#define PAT_FIT_SHRINK_ROW_PREFIX "fit_shrink,"
#define PAT_DEFAULT_FIT_SHRINK false

// Optional row: utility_adjust,V (points squared, > 0) gives every move whose
// PAT term is applied an extra
//   0.5 * V * kappa(margin + score, unseen after the move)
// where kappa = -U''/U' of the default win/spread utility (win 1.0, spread
// 0.5, scale 100) read off the letter distribution's win percentage table
// (see pat_prepare_utility) with the opponent on turn, and margin is the
// mover's lead before the move. Points and tile turnover are worth more where
// the utility is concave (ahead) than where it is convex (behind). Unlike the
// other PAT terms it can be positive, so the context folds its maximum into
// every movegen bound (see pat_eval_utility_bound). Absent means 0.
#define PAT_UTILITY_ADJUST_ROW_PREFIX "utility_adjust,"
// Training only: fit_fixed_zero,<prefix>|<prefix>|... holds every feature
// whose name starts with one of the prefixes at zero in the fit, so a model
// can be trained on a subset of premium classes. Absent means none.
#define PAT_FIT_FIXED_ZERO_ROW_PREFIX "fit_fixed_zero,"
// Margins beyond this are read at it; kappa is flat there.
#define PAT_UTILITY_MARGIN_LIMIT 600
// Finite-difference half-width, in points, for kappa: wide enough to smooth
// the table's integer margin buckets.
#define PAT_UTILITY_KAPPA_STEP 15
#define PAT_GEN_HELDOUT_EVERY 10

// Optional rows: an adjustment (milli-equity, <= 0) added to the defense
// term of an opening move, the first tile placement on an empty board, by
// the number of tiles it plays, or of an opening exchange (see
// PATWeights.opening_tiles / opening_exchange). The best length carries 0,
// which keeps the term <= 0. Absent means 0.
#define PAT_OPENING_TILES_ROW_PREFIX "opening_tiles_"
#define PAT_OPENING_EXCHANGE_ROW_PREFIX "opening_exchange,"

// Optional rows: a nonnegative factor applied to the defense term, and every
// pruning bound on it, by game stage (see PATWeights.stage_scale). The stage
// comes from the pre-move bag: early is PAT_STAGE_EARLY_MIN_BAG or more,
// late below PAT_STAGE_MID_MIN_BAG, mid in between. Absent means 1.0.
#define PAT_STAGE_SCALE_EARLY_ROW_PREFIX "stage_scale_early,"
#define PAT_STAGE_SCALE_MID_ROW_PREFIX "stage_scale_mid,"
#define PAT_STAGE_SCALE_LATE_ROW_PREFIX "stage_scale_late,"
#define PAT_DEFAULT_STAGE_SCALE 1.0
#define PAT_STAGE_EARLY_MIN_BAG 55
#define PAT_STAGE_MID_MIN_BAG 30
enum {
  PAT_STAGE_EARLY = 0,
  PAT_STAGE_MID = 1,
  PAT_STAGE_LATE = 2,
  PAT_STAGE_COUNT = 3,
};

// Divisor applied to the hook-score channel's point sum so a typical hook
// lands near the flexibility channels' magnitude.
#define PAT_HOOK_SCORE_SCALE 8

// The header line is PAT_MAGIC_PREFIX followed by the format version, e.g.
// "magpie_pat_v5". PAT_VERSION is what this build writes; an older version
// lacks rows for the channels added after it, which read as zero.
#define PAT_MAGIC_PREFIX "magpie_pat_v"
enum {
  PAT_EARLIEST_SUPPORTED_VERSION = 1,
  PAT_VERSION = 5,
};

#endif
