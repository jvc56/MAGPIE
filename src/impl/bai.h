#ifndef BAI_H
#define BAI_H

#include "../ent/random_variable.h"

#include "bai_helper.h"
#include "bai_sampling_rule.h"

int bai(bai_sampling_rule_t sr, bool is_EV, bai_threshold_t thres,
        RandomVariables *rvs, double δ, RandomVariables *rng,
        uint64_t sample_limit, BAILogger *bai_logger);

#endif