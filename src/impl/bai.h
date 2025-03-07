#ifndef BAI_H
#define BAI_H

#include "../ent/random_variable.h"

#include "bai_sampling_rule.h"

int bai(bai_sampling_rule_t sr, RandomVariables *rvs, double δ);

#endif