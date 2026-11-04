#ifndef PLAY_FEATURE_RACK_H
#define PLAY_FEATURE_RACK_H

#include "../ent/move.h"
#include "play_feature_registry.h"

// Tiles placed, or exchanged.
void play_feature_extract_tiles_played(const PlayFeatureCtx *ctx,
                                       const Move *move, double *out);
// One-hot: tile placement, exchange, pass.
void play_feature_extract_move_type(const PlayFeatureCtx *ctx, const Move *move,
                                    double *out);
// Tiles left in the bag once the player draws after the play.
void play_feature_extract_bag_after(const PlayFeatureCtx *ctx, const Move *move,
                                    double *out);
// The leave's vowels, consonants and blanks.
void play_feature_extract_leave_shape(const PlayFeatureCtx *ctx,
                                      const Move *move, double *out);
// Tiles in the leave beyond the first of each letter.
void play_feature_extract_leave_duplicates(const PlayFeatureCtx *ctx,
                                           const Move *move, double *out);

#endif
