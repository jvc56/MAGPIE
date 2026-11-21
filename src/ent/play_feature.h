#ifndef PLAY_FEATURE_H
#define PLAY_FEATURE_H

#include "../def/board_defs.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/rack.h"

// What feature extraction reads for one position. play_feature_ctx_init fills
// the common fields; a feature's prepare step fills what only it reads.
// Extraction never writes to the game.
typedef struct PlayFeatureCtx {
  const Game *game;
  const LetterDistribution *ld;
  const KLV *klv;
  // The rack of the player on turn, from which every play is made.
  Rack rack;
  int bag_tiles;
  // Filled by the premium access feature's prepare step: the empty squares
  // with a word multiplier above 1, and that multiplier.
  int num_premium_squares;
  int premium_rows[BOARD_DIM * BOARD_DIM];
  int premium_cols[BOARD_DIM * BOARD_DIM];
  int premium_word_multipliers[BOARD_DIM * BOARD_DIM];
} PlayFeatureCtx;

// Writes a feature's raw values for one play: play_feature_get_num_columns
// of them.
typedef void (*play_feature_extract_func_t)(const PlayFeatureCtx *ctx,
                                            const Move *move, double *out);

#endif
