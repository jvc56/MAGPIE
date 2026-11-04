#ifndef PLAY_FEATURE_REGISTRY_H
#define PLAY_FEATURE_REGISTRY_H

#include "../def/play_feature_defs.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include <stdbool.h>

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

void play_feature_ctx_init(PlayFeatureCtx *ctx, const Game *game);

const char *play_feature_get_name(play_feature_t feature);
int play_feature_get_num_columns(play_feature_t feature);
play_feature_kind_t play_feature_get_kind(play_feature_t feature);
// Once per position, before any extraction of the feature.
void play_feature_prepare(play_feature_t feature, PlayFeatureCtx *ctx);
// Writes the feature's columns for every play in moves: the play at index
// move_idx writes to out + move_idx * row_stride.
void play_feature_extract(play_feature_t feature, const PlayFeatureCtx *ctx,
                          const MoveList *moves, int row_stride, double *out);

// The feature with this name, or NUMBER_OF_PLAY_FEATURES if there is none.
play_feature_t play_feature_from_name(const char *name);

// Parses a comma-separated list of feature names into active (indexed by
// play_feature_t). Pushes an error naming the known features for an unknown
// name, a repeated name or an empty list.
void play_feature_parse_set(const char *names, bool *active,
                            ErrorStack *error_stack);

#endif
