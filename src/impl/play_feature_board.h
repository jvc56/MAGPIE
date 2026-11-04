#ifndef PLAY_FEATURE_BOARD_H
#define PLAY_FEATURE_BOARD_H

#include "../ent/move.h"
#include "play_feature_registry.h"

// The row and column of the middle of the tiles placed; the board's center
// for an exchange or a pass.
void play_feature_extract_location(const PlayFeatureCtx *ctx, const Move *move,
                                   double *out);
// One-hot: horizontal, vertical; both 0 for an exchange or a pass.
void play_feature_extract_direction(const PlayFeatureCtx *ctx, const Move *move,
                                    double *out);
// Lists the empty premium word squares into ctx.
void play_feature_prepare_premium_access(PlayFeatureCtx *ctx);
// The empty triple and double word squares within a rack's reach of a placed
// tile along its row or column, which the next play can more easily use.
void play_feature_extract_premium_access(const PlayFeatureCtx *ctx,
                                         const Move *move, double *out);
// The empty squares next to a placed tile: the hook spots the play opens.
void play_feature_extract_perimeter(const PlayFeatureCtx *ctx, const Move *move,
                                    double *out);

#endif
