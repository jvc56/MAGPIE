#include "play_feature_rack.h"

#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/play_feature.h"
#include "../ent/rack.h"
#include "gameplay.h"

void play_feature_extract_tiles_played(const PlayFeatureCtx
                                           __attribute__((unused)) *
                                           ctx,
                                       const Move *move, double *out) {
  out[0] = (double)move_get_tiles_played(move);
}

void play_feature_extract_move_type(const PlayFeatureCtx
                                        __attribute__((unused)) *
                                        ctx,
                                    const Move *move, double *out) {
  const game_event_t type = move_get_type(move);
  out[0] = type == GAME_EVENT_TILE_PLACEMENT_MOVE ? 1.0 : 0.0;
  out[1] = type == GAME_EVENT_EXCHANGE ? 1.0 : 0.0;
  out[2] = type == GAME_EVENT_PASS ? 1.0 : 0.0;
}

void play_feature_extract_bag_after(const PlayFeatureCtx *ctx, const Move *move,
                                    double *out) {
  int bag_after = ctx->bag_tiles;
  // An exchange returns as many tiles as it draws.
  if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    bag_after -= move_get_tiles_played(move);
  }
  out[0] = bag_after > 0 ? (double)bag_after : 0.0;
}

void play_feature_extract_leave_shape(const PlayFeatureCtx *ctx,
                                      const Move *move, double *out) {
  Rack leave;
  get_leave_for_move(move, ctx->game, &leave);
  int vowels = 0;
  int consonants = 0;
  const int dist_size = rack_get_dist_size(&leave);
  for (int ml = 1; ml < dist_size; ml++) {
    const int count = rack_get_letter(&leave, (MachineLetter)ml);
    if (ld_get_is_vowel(ctx->ld, (MachineLetter)ml)) {
      vowels += count;
    } else {
      consonants += count;
    }
  }
  out[0] = (double)vowels;
  out[1] = (double)consonants;
  out[2] = (double)rack_get_letter(&leave, BLANK_MACHINE_LETTER);
}

void play_feature_extract_leave_duplicates(const PlayFeatureCtx *ctx,
                                           const Move *move, double *out) {
  Rack leave;
  get_leave_for_move(move, ctx->game, &leave);
  int duplicates = 0;
  const int dist_size = rack_get_dist_size(&leave);
  for (int ml = 0; ml < dist_size; ml++) {
    const int count = rack_get_letter(&leave, (MachineLetter)ml);
    if (count > 1) {
      duplicates += count - 1;
    }
  }
  out[0] = (double)duplicates;
}
