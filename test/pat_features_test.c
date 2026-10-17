#include "pat_features_test.h"

#include "../src/def/pat_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_eval.h"
#include "../src/ent/pat_features.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "pat_test_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

// A lone Q on A7, directly above the A8 triple word square. CSW21's only
// Q? word is QI, so A8 hooks for a row play with exactly the unseen Is.
#define PAT_FEATURES_Q_CGP                                                     \
  "cgp 15/15/15/15/15/15/Q14/15/15/15/15/15/15/15/15 / 0/0 0"

// The features of the Q board, worked by hand. The A8 row walk starts on a
// hook square: hook_d1 is the unseen I count (9 in the bag's distribution,
// less the two on the rack). The A8 column walk meets the Q one square up
// (float_score_d1 = 10), and the A1 column walk meets it after five empty
// squares below A1 (float_score_d6 = 10). No other triple word walk reaches
// a hook or the Q.
static void test_pat_features_q_board(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, PAT_FEATURES_Q_CGP);
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(game_get_board(game), 0);
  PATWeights *pat = pat_test_create_prepared("features_q", game);
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "IIAEORS");

  int32_t features[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, rack, pat, RACK_SIZE, features);
  assert(features[PAT_FEATURE_HOOK_START] == 7);
  for (int bin = 1; bin < PAT_HOOK_BIN_COUNT; bin++) {
    assert(features[PAT_FEATURE_HOOK_START + bin] == 0);
  }
  for (int bin = 0; bin < PAT_FLOATER_BIN_COUNT; bin++) {
    const int expected = (bin == 0 || bin == 5) ? 10 : 0;
    assert(features[PAT_FEATURE_FLOAT_SCORE_START + bin] == expected);
  }
  // Without a rack every I is unseen.
  pat_extract_features(lanes, ld, NULL, pat, RACK_SIZE, features);
  assert(features[PAT_FEATURE_HOOK_START] == 9);
  // An opponent holding a single tile can still play the one-tile hook.
  pat_extract_features(lanes, ld, rack, pat, 1, features);
  assert(features[PAT_FEATURE_HOOK_START] == 7);
  assert(features[PAT_FEATURE_FLOAT_SCORE_START + 5] == 0);

  rack_destroy(rack);
  pat_destroy(pat);
  config_destroy(config);
}

// The training row for combined units: gamma times the sum of every unit's
// row plus (1 - gamma) of the worst unit's row, so its dot product with the
// weights is the combined term evaluation applies. On the Q board with
// weights on hook_d1 (-1 point per unseen I) and float_score_d1 (-0.001
// per point), the A8 row walk is the worst unit (-7000) and the A8 column
// walk (-10) the only other one charged: combined -7005 at gamma 0.5.
// Untrained weights have no worst unit, and the row is the plain sum.
static void test_pat_features_combined_row(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, PAT_FEATURES_Q_CGP);
  const Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  const Square *lanes = board_get_readonly_lanes(game_get_board(game), 0);
  PATWeights *pat = pat_test_create_prepared("features_combined", game);
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "IIAEORS");

  int32_t raw[PAT_NUM_FEATURES];
  pat_extract_features(lanes, ld, rack, pat, RACK_SIZE, raw);
  double combined[PAT_NUM_FEATURES];
  pat_extract_features_combined(lanes, ld, rack, pat, RACK_SIZE, combined);
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    assert(combined[feature_index] == (double)raw[feature_index]);
  }

  pat_set_combine_gamma(pat, 0.5);
  pat_set_weight(pat, PAT_FEATURE_HOOK_START, -1000);
  pat_set_weight(pat, PAT_FEATURE_FLOAT_SCORE_START, -1);
  pat_extract_features_combined(lanes, ld, rack, pat, RACK_SIZE, combined);
  // hook_d1 lies wholly in the worst unit; the floater scores lie in
  // units that are not the worst, so only gamma of them counts.
  assert(combined[PAT_FEATURE_HOOK_START] == 7.0);
  assert(combined[PAT_FEATURE_HOOK_SCORE_START] ==
         (double)raw[PAT_FEATURE_HOOK_SCORE_START]);
  assert(combined[PAT_FEATURE_FLOAT_SCORE_START] == 5.0);
  assert(combined[PAT_FEATURE_FLOAT_SCORE_START + 5] == 5.0);
  double dot = 0.0;
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    dot += (double)pat_get_weight(pat, feature_index) * combined[feature_index];
  }
  assert(fabs(dot - -7005.0) < 1e-9);
  PATEvalContext *pat_eval_ctx = malloc_or_die(sizeof(PATEvalContext));
  pat_eval_context_load(pat_eval_ctx, pat, lanes, ld, rack, PAT_CLASS_MASK_ALL,
                        RACK_SIZE);
  assert(pat_eval_non_placement_penalty(pat_eval_ctx) == -7005);
  free(pat_eval_ctx);

  rack_destroy(rack);
  pat_destroy(pat);
  config_destroy(config);
}

void test_pat_features(void) {
  test_pat_features_q_board();
  test_pat_features_combined_row();
}
