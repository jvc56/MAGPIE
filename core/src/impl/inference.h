#ifndef INFERENCE_H
#define INFERENCE_H

#include "../def/inference_defs.h"

#include "../util/log.h"

#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/inference_results.h"
#include "../ent/leave_rack.h"
#include "../ent/stats.h"
#include "../ent/thread_control.h"

double get_probability_for_random_minimum_draw(
    const Rack *bag_as_rack, const Rack *target_rack, uint8_t this_letter,
    int minimum, int number_of_target_played_tiles);

uint64_t choose(uint64_t n, uint64_t k);

inference_status_t infer(const Config *config, Game *game,
                         InferenceResults **inference_results);
#endif