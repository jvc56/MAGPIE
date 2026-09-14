#ifndef PAT_DEFS_H
#define PAT_DEFS_H

#include "rack_defs.h"

// Feature layout for the PAT weights (see src/ent/pat.h).
//
// Every feature measures one channel of opponent access to an empty triple
// word square, binned by the number of empty squares a word reaching the TWS
// must fill ("d"), which is the number of tiles the opponent must play. Both
// channels start at d = 1: a hook at d = 1 is a hookable TWS square itself (a
// one-tile triple play), and a floater at d = 1 is a playthrough tile directly
// adjacent to the TWS.
enum {
  PAT_HOOK_BIN_COUNT = RACK_SIZE,
  PAT_FLOATER_BIN_COUNT = RACK_SIZE,
  PAT_FEATURE_HOOK_START = 0,
  PAT_FEATURE_FLOAT_FLEX_START = PAT_FEATURE_HOOK_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_FLOAT_SCORE_START =
      PAT_FEATURE_FLOAT_FLEX_START + PAT_FLOATER_BIN_COUNT,
  // Face value alone does not say what a floater is worth to the opponent.
  // What matters is the words that actually reach the triple through it and
  // what they score on the way: a J prices high and reaches almost nothing,
  // an S prices at one and reaches nearly everything, and a blank scores
  // nothing itself while reaching whatever the letter it was played as
  // reaches. Both channels come from a table built once from the lexicon,
  // keyed by the floater's letter and how far the word has to span to
  // arrive: the mean tile value a reaching word lays down besides the
  // floater, and how many such words exist at all.
  PAT_FEATURE_FLOAT_THROUGH_SCORE_START =
      PAT_FEATURE_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_FLOAT_THROUGH_COUNT_START =
      PAT_FEATURE_FLOAT_THROUGH_SCORE_START + PAT_FLOATER_BIN_COUNT,
  // Raw unseen counts (above) implicitly treat every unseen copy of a
  // letter as equally likely to already be in the opponent's hand, which
  // overstates risk whenever the bag is still large relative to their
  // rack: most unseen copies are sitting in the bag, not their hand, yet.
  // These channels are the same hook and floater-flex sums scaled by the
  // hypergeometric expectation opponent_rack_size / total_unseen, so they
  // shrink early (large bag) and rise toward the raw count late (bag
  // nearly empty) instead of treating both alike. Present from format
  // version 3 on; earlier files have no rows for these and they read as
  // zero (see pat_parse_contents). TWS-only, like the through-table:
  // spending the feature budget on a scaled variant of every class was
  // not worth it before knowing whether scaling helps at all.
  PAT_FEATURE_HOOK_SCALED_START =
      PAT_FEATURE_FLOAT_THROUGH_COUNT_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_FLOAT_FLEX_SCALED_START =
      PAT_FEATURE_HOOK_SCALED_START + PAT_HOOK_BIN_COUNT,
  // Hook flexibility counts every unseen tile that fits a hook alike; a
  // hook that takes a J is a bigger immediate threat than one that takes
  // an S, and hooking a long high-scoring word is worse than hooking AT.
  // This channel is the flexibility sum weighted by each admissible
  // letter's incremental immediate score with the real geometry: the
  // hooked word's existing score under the hook square's word
  // multiplier, plus the letter's own value under the square's letter
  // multiplier, counted in the hook word and again in the lane word the
  // triple multiplies (see pat_effective_cross_info), divided by
  // PAT_HOOK_SCORE_SCALE to keep it on the other channels' scale. Present
  // from format version 4 on. TWS-only, like the through table.
  PAT_FEATURE_HOOK_SCORE_START =
      PAT_FEATURE_FLOAT_FLEX_SCALED_START + PAT_FLOATER_BIN_COUNT,
  // The same hook and floater-value channels for the lesser premium
  // squares. A lone double word square doubles a whole word and a triple
  // letter square triples one tile, so both are worth reaching and both
  // were invisible to a model that only ever walked triple-word lanes.
  // They get their own channels rather than a shared one scaled by a
  // guessed multiplier, so the fit says what each class is worth.
  PAT_FEATURE_DWS_HOOK_START =
      PAT_FEATURE_HOOK_SCORE_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_DWS_FLOAT_SCORE_START =
      PAT_FEATURE_DWS_HOOK_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_TLS_HOOK_START =
      PAT_FEATURE_DWS_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_TLS_FLOAT_SCORE_START =
      PAT_FEATURE_TLS_HOOK_START + PAT_HOOK_BIN_COUNT,
  // Double letter squares: the most numerous premium square on the
  // standard board (24 of them, twice the triple letters), left out of
  // every earlier model because walking them nearly doubled the scan cost
  // for what a single extra letter's worth of multiplier seemed likely to
  // be worth. Present from format version 2 on; a version 1 file has no
  // rows for these and they read as zero (see pat_parse_contents).
  PAT_FEATURE_DLS_HOOK_START =
      PAT_FEATURE_TLS_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_DLS_FLOAT_SCORE_START =
      PAT_FEATURE_DLS_HOOK_START + PAT_HOOK_BIN_COUNT,
  // The super board's quadruple word and quadruple letter squares. The
  // four quad-word corners are its most valuable squares by a distance and
  // were invisible to a scan that knew multipliers only up to three.
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
  // Windows: a pair of empty word-multiplier squares in one lane close
  // enough for a single word to cover both, which multiplies the word by
  // the product of the two. On the standard board the only such pairs are
  // double-doubles; the super board adds double-triple, triple-triple and
  // triple-quad windows, so windows are binned by that product. A window
  // is counted as floater access when a playthrough tile sits inside it and
  // as hook-only access when it is empty but has a hookable square,
  // mirroring the triple-triple pair, and tiles saved is the rack the
  // opponent does NOT have to spend (RACK_SIZE minus the empty squares the
  // window still needs), so it rises with how reachable the window is.
  PAT_WINDOW_TIER_COUNT = 4,
  PAT_WINDOW_FEATURES_PER_TIER = 3,
  PAT_FEATURE_WINDOW_START = PAT_FEATURE_TT_HOOK_ONLY + 1,
  // Tier 0 is the double-double, kept under its old names.
  PAT_FEATURE_DD_FLOATER = PAT_FEATURE_WINDOW_START,
  PAT_FEATURE_DD_HOOK_ONLY = PAT_FEATURE_DD_FLOATER + 1,
  PAT_FEATURE_DD_TILES_SAVED = PAT_FEATURE_DD_HOOK_ONLY + 1,
  PAT_NUM_FEATURES = PAT_FEATURE_WINDOW_START +
                     PAT_WINDOW_TIER_COUNT * PAT_WINDOW_FEATURES_PER_TIER,
};

// The label a training observation carries is the opponent's net gain over
// the next this-many plies, so the buffer holds at most this many
// observations waiting to be labeled at once.
#define PAT_MAX_LABEL_PLIES 6

// Optional row naming how per-unit penalties combine; absent means 1.0,
// the plain sum every earlier file used.
// The through-table spans word lengths 2 up to this; a floater further
// from the triple than this cannot be reached by one word anyway.
#define PAT_MAX_THROUGH_LEN 16

#define PAT_GAMMA_ROW_PREFIX "gamma,"
#define PAT_DEFAULT_COMBINE_GAMMA 1.0
// What new training uses unless told otherwise. Measured flat between 0.4
// and 0.7, so this is the middle of a plateau rather than a peak, and it
// reads as a second route to danger being worth half a first.
#define PAT_TRAINING_COMBINE_GAMMA 0.5

// Optional row giving the fraction of a unit's penalty credited back when
// the move's own leave holds a letter that could exploit that unit itself
// (see PATWeights.own_asset_discount); absent means 0.0, no credit, the
// behavior every earlier file already has.
#define PAT_OWN_ASSET_DISCOUNT_ROW_PREFIX "own_asset_discount,"
#define PAT_DEFAULT_OWN_ASSET_DISCOUNT 0.0

// Optional row (0 or 1) selecting which letters a floater run's flexibility
// counts (see PATWeights.lexicon_floaters). Absent means 0: the behavior
// every earlier file was trained under, where a floater's flexibility is
// every unseen tile regardless of what the lexicon lets extend the run.
#define PAT_LEXICON_FLOATERS_ROW_PREFIX "lexicon_floaters,"
#define PAT_DEFAULT_LEXICON_FLOATERS false

// Optional row (0 or 1) selecting whether the floater through-table
// statistics distinguish which end of the opponent's word the floater
// would be (see PATWeights.signed_through). Absent means 0: the unsigned
// tables every earlier file was trained under.
#define PAT_SIGNED_THROUGH_ROW_PREFIX "signed_through,"
#define PAT_DEFAULT_SIGNED_THROUGH false

// Optional row (0 or 1): whether patgen fits the hypergeometric-scaled
// channels (hook_scaled_d*, float_flex_scaled_d*) or excludes them from
// the regression, leaving their weights at zero (see
// PATWeights.fit_scaled_channels). Absent means 1.
#define PAT_FIT_SCALED_ROW_PREFIX "fit_scaled,"
#define PAT_DEFAULT_FIT_SCALED true

// Optional row (0 or 1): whether patgen builds each training row through
// the runtime overlay path from the pre-move context
// (pat_extract_move_features_combined) instead of scanning the post-move
// board (see PATWeights.train_overlay). Absent means 0, the post-move
// scan every earlier file was trained on.
#define PAT_TRAIN_OVERLAY_ROW_PREFIX "train_overlay,"
#define PAT_DEFAULT_TRAIN_OVERLAY false

// Optional row (0 or 1): whether patgen fits only the hook-score channels
// (PAT_FEATURE_HOOK_SCORE_START onward, PAT_HOOK_BIN_COUNT of them) as a
// residual on top of the file's other weights, which stay exactly as
// loaded (see PATWeights.fit_residual). Absent means 0: every channel
// fitted.
#define PAT_FIT_RESIDUAL_ROW_PREFIX "fit_residual,"
#define PAT_DEFAULT_FIT_RESIDUAL false

// Optional row (0 or 1): whether a hook the evaluated move itself creates
// is scored from its real cross set and cross score, resolved on the
// GADDAG from the perpendicular pattern the move forms, instead of the
// per-letter two-letter-word count approximation (see
// PATWeights.exact_created_hooks). Absent means 0, the approximation
// every file so far was evaluated with. Needs the context's KWG
// (pat_eval_context_set_kwg); without it the approximation is used.
#define PAT_EXACT_CREATED_HOOKS_ROW_PREFIX "exact_created_hooks,"
#define PAT_DEFAULT_EXACT_CREATED_HOOKS false

// Divisor applied to the hook-score channel's availability-weighted point
// sum so a typical hook lands near the flexibility channels' magnitude.
#define PAT_HOOK_SCORE_SCALE 8

// The header line is PAT_MAGIC_PREFIX followed by the format version as a
// decimal integer, e.g. "magpie_pat_v2". PAT_VERSION is what this build
// writes; a file naming a version below PAT_EARLIEST_SUPPORTED_VERSION is
// rejected (see the PAT_UNSUPPORTED_VERSION error). Version 1 predates the
// double letter square channels (PAT_FEATURE_DLS_HOOK_START onward), and
// version 2 predates the hypergeometric-scaled channels
// (PAT_FEATURE_HOOK_SCALED_START onward), and version 3 the hook-score
// channels (PAT_FEATURE_HOOK_SCORE_START onward): an older file has no
// rows for the channels added after it and they read as zero, so raising
// PAT_EARLIEST_SUPPORTED_VERSION is never required by adding a class.
#define PAT_MAGIC_PREFIX "magpie_pat_v"
enum {
  PAT_EARLIEST_SUPPORTED_VERSION = 1,
  PAT_VERSION = 4,
};

#endif
