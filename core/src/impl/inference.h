#ifndef INFERENCE_H
#define INFERENCE_H

#include "../def/inference_defs.h"

#include "../util/log.h"

#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"

inference_status_t infer(const Config *config, const Game *game,
                         InferenceResults *inference_results);
#endif