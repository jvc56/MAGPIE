#ifndef INFERENCE_H
#define INFERENCE_H

#include "../def/inference_defs.h"

#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/rack.h"

#include "config.h"

inference_status_t infer(const Config *config, const Game *input_game,
                         int target_index, Rack *target_rack, int target_score,
                         int target_num_exch, InferenceResults *results);
#endif