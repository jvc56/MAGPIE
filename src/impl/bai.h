#ifndef BAI_H
#define BAI_H

#include "../ent/random_variable.h"

#include "bai_helper.h"
#include "bai_sampling_rule.h"

int bai(const bai_sampling_rule_t sr, const bool is_EV,
        const bai_threshold_t thres, RandomVariables *rvs, const double δ,
        RandomVariables *rng, const int sample_limit,
        const int similar_play_min_iter_for_eval, BAILogger *bai_logger);

#endif