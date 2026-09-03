#ifndef TWS_DEFENSE_DEFS_H
#define TWS_DEFENSE_DEFS_H

#include "rack_defs.h"

// Feature layout for the TWS defense weights (see src/ent/tws_defense.h).
//
// Every feature measures one channel of opponent access to an empty triple
// word square, binned by the number of empty squares a word reaching the TWS
// must fill ("d"), which is the number of tiles the opponent must play. Both
// channels start at d = 1: a hook at d = 1 is a hookable TWS square itself (a
// one-tile triple play), and a floater at d = 1 is a playthrough tile directly
// adjacent to the TWS.
enum {
  TWD_HOOK_BIN_COUNT = RACK_SIZE,
  TWD_FLOATER_BIN_COUNT = RACK_SIZE,
  TWD_FEATURE_HOOK_START = 0,
  TWD_FEATURE_FLOAT_FLEX_START = TWD_FEATURE_HOOK_START + TWD_HOOK_BIN_COUNT,
  TWD_FEATURE_FLOAT_SCORE_START =
      TWD_FEATURE_FLOAT_FLEX_START + TWD_FLOATER_BIN_COUNT,
  // Face value alone does not say what a floater is worth to the opponent.
  // What matters is the words that actually reach the triple through it and
  // what they score on the way: a J prices high and reaches almost nothing,
  // an S prices at one and reaches nearly everything, and a blank scores
  // nothing itself while reaching whatever the letter it was played as
  // reaches. Both channels come from a table built once from the lexicon,
  // keyed by the floater's letter and how far the word has to span to
  // arrive: the mean tile value a reaching word lays down besides the
  // floater, and how many such words exist at all.
  TWD_FEATURE_FLOAT_THROUGH_SCORE_START =
      TWD_FEATURE_FLOAT_SCORE_START + TWD_FLOATER_BIN_COUNT,
  TWD_FEATURE_FLOAT_THROUGH_COUNT_START =
      TWD_FEATURE_FLOAT_THROUGH_SCORE_START + TWD_FLOATER_BIN_COUNT,
  // The same hook and floater-value channels for the lesser premium
  // squares. A lone double word square doubles a whole word and a triple
  // letter square triples one tile, so both are worth reaching and both
  // were invisible to a model that only ever walked triple-word lanes.
  // They get their own channels rather than a shared one scaled by a
  // guessed multiplier, so the fit says what each class is worth.
  TWD_FEATURE_DWS_HOOK_START =
      TWD_FEATURE_FLOAT_THROUGH_COUNT_START + TWD_FLOATER_BIN_COUNT,
  TWD_FEATURE_DWS_FLOAT_SCORE_START =
      TWD_FEATURE_DWS_HOOK_START + TWD_HOOK_BIN_COUNT,
  TWD_FEATURE_TLS_HOOK_START =
      TWD_FEATURE_DWS_FLOAT_SCORE_START + TWD_FLOATER_BIN_COUNT,
  TWD_FEATURE_TLS_FLOAT_SCORE_START =
      TWD_FEATURE_TLS_HOOK_START + TWD_HOOK_BIN_COUNT,
  // The super board's quadruple word and quadruple letter squares. The
  // four quad-word corners are its most valuable squares by a distance and
  // were invisible to a scan that knew multipliers only up to three.
  TWD_FEATURE_QWS_HOOK_START =
      TWD_FEATURE_TLS_FLOAT_SCORE_START + TWD_FLOATER_BIN_COUNT,
  TWD_FEATURE_QWS_FLOAT_SCORE_START =
      TWD_FEATURE_QWS_HOOK_START + TWD_HOOK_BIN_COUNT,
  TWD_FEATURE_QLS_HOOK_START =
      TWD_FEATURE_QWS_FLOAT_SCORE_START + TWD_FLOATER_BIN_COUNT,
  TWD_FEATURE_QLS_FLOAT_SCORE_START =
      TWD_FEATURE_QLS_HOOK_START + TWD_HOOK_BIN_COUNT,
  TWD_FEATURE_TT_FLOATER =
      TWD_FEATURE_QLS_FLOAT_SCORE_START + TWD_FLOATER_BIN_COUNT,
  TWD_FEATURE_TT_HOOK_ONLY = TWD_FEATURE_TT_FLOATER + 1,
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
  TWD_WINDOW_TIER_COUNT = 4,
  TWD_WINDOW_FEATURES_PER_TIER = 3,
  TWD_FEATURE_WINDOW_START = TWD_FEATURE_TT_HOOK_ONLY + 1,
  // Tier 0 is the double-double, kept under its old names.
  TWD_FEATURE_DD_FLOATER = TWD_FEATURE_WINDOW_START,
  TWD_FEATURE_DD_HOOK_ONLY = TWD_FEATURE_DD_FLOATER + 1,
  TWD_FEATURE_DD_TILES_SAVED = TWD_FEATURE_DD_HOOK_ONLY + 1,
  TWD_NUM_FEATURES = TWD_FEATURE_WINDOW_START +
                     TWD_WINDOW_TIER_COUNT * TWD_WINDOW_FEATURES_PER_TIER,
};

// The label a training observation carries is the opponent's net gain over
// the next this-many plies, so the buffer holds at most this many
// observations waiting to be labeled at once.
#define TWD_MAX_LABEL_PLIES 4

// Optional row naming how per-unit penalties combine; absent means 1.0,
// the plain sum every earlier file used.
// The through-table spans word lengths 2 up to this; a floater further
// from the triple than this cannot be reached by one word anyway.
#define TWD_MAX_THROUGH_LEN 16

#define TWD_GAMMA_ROW_PREFIX "gamma,"
#define TWD_DEFAULT_COMBINE_GAMMA 1.0
// What new training uses unless told otherwise. Measured flat between 0.4
// and 0.7, so this is the middle of a plateau rather than a peak, and it
// reads as a second route to danger being worth half a first.
#define TWD_TRAINING_COMBINE_GAMMA 0.5

#define TWD_MAGIC_HEADER "magpie_twd_v2"

#endif
