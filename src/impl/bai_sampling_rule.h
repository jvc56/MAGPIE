#ifndef BAI_SAMPLING_RULE_H
#define BAI_SAMPLING_RULE_H

#include "../def/bai_defs.h"

#include "bai_logger.h"

#include "random_variable.h"

typedef struct BAISamplingRule BAISamplingRule;

BAISamplingRule *bai_sampling_rule_create(const bai_sampling_rule_t type,
                                          const int *N, const int size);
void bai_sampling_rule_destroy(BAISamplingRule *bai_sampling_rule);
int bai_sampling_rule_next_sample(const BAISamplingRule *bai_sampling_rule,
                                  const int astar, const int aalt,
                                  const double *ξ, const double *ϕ2,
                                  const int *N, const double *S,
                                  const double *Zs, const int size,
                                  RandomVariables *rng, BAILogger *bai_logger);
void bai_sampling_rule_swap_indexes(BAISamplingRule *bai_sampling_rule,
                                    const int i, const int j,
                                    BAILogger *bai_logger);
#endif