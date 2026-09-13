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
  // The same hook and floater-value channels for the lesser premium
  // squares. A lone double word square doubles a whole word and a triple
  // letter square triples one tile, so both are worth reaching and both
  // were invisible to a model that only ever walked triple-word lanes.
  // They get their own channels rather than a shared one scaled by a
  // guessed multiplier, so the fit says what each class is worth.
  PAT_FEATURE_DWS_HOOK_START =
      PAT_FEATURE_FLOAT_THROUGH_COUNT_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_DWS_FLOAT_SCORE_START =
      PAT_FEATURE_DWS_HOOK_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_TLS_HOOK_START =
      PAT_FEATURE_DWS_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
  PAT_FEATURE_TLS_FLOAT_SCORE_START =
      PAT_FEATURE_TLS_HOOK_START + PAT_HOOK_BIN_COUNT,
  // The super board's quadruple word and quadruple letter squares. The
  // four quad-word corners are its most valuable squares by a distance and
  // were invisible to a scan that knew multipliers only up to three.
  PAT_FEATURE_QWS_HOOK_START =
      PAT_FEATURE_TLS_FLOAT_SCORE_START + PAT_FLOATER_BIN_COUNT,
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
#define PAT_MAX_LABEL_PLIES 4

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

// The header line is PAT_MAGIC_PREFIX followed by the format version as a
// decimal integer, e.g. "magpie_pat_v1". PAT_VERSION is what this build
// writes; a file naming a version below PAT_EARLIEST_SUPPORTED_VERSION is
// rejected (see the PAT_UNSUPPORTED_VERSION error).
#define PAT_MAGIC_PREFIX "magpie_pat_v"
enum {
  PAT_EARLIEST_SUPPORTED_VERSION = 1,
  PAT_VERSION = 1,
};

#endif
