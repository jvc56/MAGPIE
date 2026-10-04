#ifndef VALUE_NET_PLAYER_H
#define VALUE_NET_PLAYER_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../util/io_util.h"
#include "value_net_features.h"
#include <stdbool.h>

// Chooses moves as Macondo's FastMlBot does: the top max_candidates
// (default 50) moves by static equity, one value net row each, one batched
// evaluation, and the one with the highest utility played (ties: more tiles
// played). The utility is MAGPIE's score+win blend with the player's
// weights (value_net_utility: win% from the net's value, final spread from
// its spread head). With an empty bag, the top static move.
typedef enum {
  VALUE_NET_BACKEND_CPU,
  VALUE_NET_BACKEND_METAL_FP32,
  VALUE_NET_BACKEND_METAL_FP16,
  // The Neural Engine, through the CoreML build at <model_dir>/ane.mlpackage.
  VALUE_NET_BACKEND_COREML,
  // Both: each evaluation on the GPU (fp16) while it has a free slot, else
  // on the Neural Engine while it has one, else queued for the GPU.
  VALUE_NET_BACKEND_COREML_AND_METAL,
} value_net_backend_t;

typedef struct ValueNetPlayer ValueNetPlayer;

// utility_w_winpct, utility_w_spread and utility_spread_scale are the
// utility weights as in SimArgs.
ValueNetPlayer *
value_net_player_create(const char *model_dir, value_net_backend_t backend,
                        int max_candidates, double utility_w_winpct,
                        double utility_w_spread, double utility_spread_scale,
                        ErrorStack *error_stack);
void value_net_player_destroy(ValueNetPlayer *player);

// Evaluates rows input rows into values and, unless spreads is NULL,
// spreads (a value_net_rows_fn, context being the ValueNetPlayer); safe to
// call from several threads.
void value_net_player_evaluate_rows(void *context, int rows, const float *board,
                                    const float *scalars, float *values,
                                    float *spreads);

// Whether the player's net has an opponent-leave head (value_net.h).
bool value_net_player_has_rack_head(const ValueNetPlayer *player);

// The opponent-leave head's log odds for rows input rows with side inputs
// (a value_net_rack_odds_fn, context being the ValueNetPlayer): the hidden
// vectors come from the GPU when the player has it, else the CPU. Safe to
// call from several threads.
void value_net_player_rack_odds(void *context, int rows, const float *board,
                                const float *scalars, const float *side,
                                float *theta);

// The chosen move for the player on turn in game, whose history (see
// ValueNetHistory) is history; valid until the next call.
const Move *value_net_player_choose(ValueNetPlayer *player, const Game *game,
                                    const ValueNetHistory *history);

// A cascade: after player's net ranks the candidates, rescorer's net
// evaluates the top rescore_top of them (at most rescorer's
// max_candidates), and player plays the best of those by player's utility
// from rescorer's outputs. The player does not own rescorer. NULL turns
// rescoring off.
void value_net_player_set_rescorer(ValueNetPlayer *player,
                                   ValueNetPlayer *rescorer, int rescore_top);

#endif
