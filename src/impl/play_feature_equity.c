#include "play_feature_equity.h"

#include "../def/game_history_defs.h"
#include "../ent/equity.h"
#include "../ent/klv.h"
#include "../ent/move.h"
#include "../ent/play_feature.h"
#include "../ent/rack.h"
#include "../ent/static_eval.h"
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
  // With tiles in the bag the rack is full and the KLV, which holds leaves
  // of at most 6 tiles, values it at 0, as it values a bingo's empty leave
  // (so the pass's leave feature is 0 too). With the bag empty it is valued
  // as move generation values a play that does not go out.
  if (move_get_type(move) == GAME_EVENT_PASS) {
    const Equity kept = ctx->bag_tiles > 0
                            ? klv_get_leave_value(ctx->klv, &ctx->rack)
                            : endgame_nonoutplay_adjustment(
                                  rack_get_score(ctx->ld, &ctx->rack));
    out[0] = equity_to_double(kept);
    return;
  }
  out[0] = equity_to_double(move_get_equity(move));
}
