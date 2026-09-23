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
  // Premium combinations along a lane: a word reaching an open word
  // multiplier may also lay a tile on a letter multiplier on the way (row
  // 1 has double letters at D1 and L1 between the triples; the double-
  // word lanes carry triple letters), which is where a high tile scores
  // 50 or more. Per route the scan takes the largest letter multiplier
  // among the EMPTY squares the reaching word must cover (the premium,
  // the empties to the route's contact, and a hook square itself; an
  // occupied letter square is spent) and adds word multiplier x (letter
  // multiplier - 1), binned by the route's distance like every other
  // channel ("span"). A letter multiplier the word would only reach by
  // extending past its contact, or past the premium on the far side,
  // within the rack budget, is counted separately ("ext"), and only when
  // it beats what the span already has. Routes are counted, not
  // flexibility-weighted, and only feasible ones (some unseen tile fits).
  // Triple and double word lanes each get their own channels, appended
  // after the windows so an older file simply ends before them. Present
  // from format version 5 on.
  PAT_FEATURE_LM_SPAN_START =
      PAT_FEATURE_WINDOW_START +
      PAT_WINDOW_TIER_COUNT * PAT_WINDOW_FEATURES_PER_TIER,
  PAT_FEATURE_LM_EXT_START = PAT_FEATURE_LM_SPAN_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_DWS_LM_SPAN_START = PAT_FEATURE_LM_EXT_START + PAT_HOOK_BIN_COUNT,
  PAT_FEATURE_DWS_LM_EXT_START =
      PAT_FEATURE_DWS_LM_SPAN_START + PAT_HOOK_BIN_COUNT,
  PAT_NUM_FEATURES = PAT_FEATURE_DWS_LM_EXT_START + PAT_HOOK_BIN_COUNT,
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

// Optional row (0 to 5): whether patgen fits only the hook-score channels
// (PAT_FEATURE_HOOK_SCORE_START onward, PAT_HOOK_BIN_COUNT of them) as a
// residual on top of the file's other weights, which stay exactly as
// loaded (1), those plus the triple-word hook flexibility channels (2),
// only the floater through channels (3), or only the premium-combination
// channels (PAT_FEATURE_LM_SPAN_START onward) (4); mode 5 fits all regular
// channels including hook-score, for an explicit experimental full refit. See
// PATWeights.fit_residual. Absent means 0: regular channels fit while
// hook-score channels stay fixed at their loaded values.
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

// Optional row (0 or 1): whether the floater through channels score a
// run of tiles by the words that actually contain the whole run at the
// required end (a run-keyed table, up to PAT_RUN_THROUGH_MAX_KEY letters
// deep) instead of summing each tile's single-letter statistic (see
// PATWeights.run_through). Absent means 0, the per-tile sum every file so
// far was trained on.
#define PAT_RUN_THROUGH_ROW_PREFIX "run_through,"
#define PAT_DEFAULT_RUN_THROUGH false
// Runs longer than this are keyed by their far-end letters, whose word
// count is a superset of the run's.
#define PAT_RUN_THROUGH_MAX_KEY 3

// Optional row (0 or 1): whether patgen produces adaptation candidates
// instead of refitting -- for each of a small set of shrinkage strengths
// every free coefficient is pulled toward the value the file was loaded
// with, and one candidate file per strength is written (infinity being
// the incumbent itself) with its validation reply-prediction error; the
// OUTPUT is the loaded weights unchanged, because validation error does
// not select a playing strength (see test/pat_select_champion.sh for the
// whole-game selection). About one in PAT_GEN_HELDOUT_EVERY game pairs,
// by a hash of the pair number, is the validation set. Absent means 0:
// the plain fit every file so far was trained with.
#define PAT_FIT_SHRINK_ROW_PREFIX "fit_shrink,"
#define PAT_DEFAULT_FIT_SHRINK false
#define PAT_GEN_HELDOUT_EVERY 10

// Optional rows: a nonnegative factor applied to the whole defense term,
// and to every pruning bound on it, by game stage (see
// PATWeights.stage_scale). The stage comes from the pre-move bag count:
// early is a bag of PAT_STAGE_EARLY_MIN_BAG or more, late one below
// PAT_STAGE_MID_MIN_BAG, mid in between -- the ranges the stage
// diagnostics were run over. Absent means 1.0: the term as trained. The
// weights themselves are never scaled, so training and the features are
// untouched; this is purely how strongly a trained term is applied.
// Optional rows: an adjustment (milli-equity, <= 0) added to the defense
// term of an opening move -- the first tile placement on an empty board,
// by the number of tiles it plays, or an opening exchange (see
// PATWeights.opening_tiles / opening_exchange). What static evaluation
// systematically misses about opening length, measured by simulation
// and by whole-game residuals; entries are relative, so the best length
// carries 0 and the rest are penalties, which keeps the whole term <= 0
// and its omission from the shadow bounds sound. Absent means 0.
#define PAT_OPENING_TILES_ROW_PREFIX "opening_tiles_"
#define PAT_OPENING_EXCHANGE_ROW_PREFIX "opening_exchange,"

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

// Divisor applied to the hook-score channel's availability-weighted point
// sum so a typical hook lands near the flexibility channels' magnitude.
#define PAT_HOOK_SCORE_SCALE 8

// The header line is PAT_MAGIC_PREFIX followed by the format version as a
// decimal integer, e.g. "magpie_pat_v2". PAT_VERSION is what this build
// writes; a file naming a version below PAT_EARLIEST_SUPPORTED_VERSION is
// rejected (see the PAT_UNSUPPORTED_VERSION error). Version 1 predates the
// double letter square channels (PAT_FEATURE_DLS_HOOK_START onward), and
// version 2 predates the hypergeometric-scaled channels
// (PAT_FEATURE_HOOK_SCALED_START onward), version 3 the hook-score
// channels (PAT_FEATURE_HOOK_SCORE_START onward), and version 4 the
// premium-combination channels (PAT_FEATURE_LM_SPAN_START onward): an
// older file has no rows for the channels added after it and they read
// as zero, so raising PAT_EARLIEST_SUPPORTED_VERSION is never required by
// adding a class.
#define PAT_MAGIC_PREFIX "magpie_pat_v"
enum {
  PAT_EARLIEST_SUPPORTED_VERSION = 1,
  PAT_VERSION = 5,
};

#endif
