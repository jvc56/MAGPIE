#ifndef BAI_H
#define BAI_H

#include "../def/bai_defs.h"

#include "../ent/thread_control.h"

#include "random_variable.h"

void bai(const BAIOptions *bai_options, RandomVariables *rvs,
         RandomVariables *rng, ThreadControl *thread_control,
         BAILogger *bai_logger, BAIResult *bai_result);

#endif