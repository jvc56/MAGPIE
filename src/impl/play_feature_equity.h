#ifndef PLAY_FEATURE_EQUITY_H
#define PLAY_FEATURE_EQUITY_H

#include "../ent/move.h"
#include "../ent/play_feature.h"

// The play's score, in points.
void play_feature_extract_score(const PlayFeatureCtx *ctx, const Move *move,
                                double *out);
// The leave's value from the player's KLV, in points.
void play_feature_extract_leave_value(const PlayFeatureCtx *ctx,
                                      const Move *move, double *out);
// The play's static equity, in points; for a pass, the rack's leave value.
void play_feature_extract_equity(const PlayFeatureCtx *ctx, const Move *move,
                                 double *out);

#endif
