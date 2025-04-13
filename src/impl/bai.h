#ifndef BAI_H
#define BAI_H

#include "../def/bai_defs.h"

#include "../ent/random_variable.h"

void bai(const BAIOptions *bai_options, RandomVariables *rvs,
         RandomVariables *rng, BAILogger *bai_logger, BAIResult *bai_result);

#endif