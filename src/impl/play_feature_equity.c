#include "play_feature_equity.h"

#include "../def/game_history_defs.h"
#include "../ent/equity.h"
#include "../ent/klv.h"
#include "../ent/move.h"
#include "../ent/play_feature.h"
#include "../ent/rack.h"
#include "gameplay.h"

void play_feature_extract_score(const PlayFeatureCtx __attribute__((unused)) *
                                    ctx,
                                const Move *move, double *out) {
  out[0] = equity_to_double(move_get_score(move));
}

void play_feature_extract_leave_value(const PlayFeatureCtx *ctx,
                                      const Move *move, double *out) {
  Rack leave;
  get_leave_for_move(move, ctx->game, &leave);
  out[0] = equity_to_double(klv_get_leave_value(ctx->klv, &leave));
}

void play_feature_extract_equity(const PlayFeatureCtx *ctx, const Move *move,
                                 double *out) {
  // A pass's equity is a sentinel; it scores nothing and keeps the rack.
  if (move_get_type(move) == GAME_EVENT_PASS) {
    out[0] = equity_to_double(klv_get_leave_value(ctx->klv, &ctx->rack));
    return;
  }
  out[0] = equity_to_double(move_get_equity(move));
}
