#ifndef BAI_SAMPLING_RULE_H
#define BAI_SAMPLING_RULE_H

#include "../ent/bai_logger.h"
#include "../ent/random_variable.h"

typedef struct BAISamplingRule BAISamplingRule;

typedef enum {
  BAI_SAMPLING_RULE_ROUND_ROBIN,
  BAI_SAMPLING_RULE_TRACK_AND_STOP,
  BAI_SAMPLING_RULE_TOP_TWO,
} bai_sampling_rule_t;

BAISamplingRule *bai_sampling_rule_create(const bai_sampling_rule_t type,
                                          const bool is_EV, const int *N,
                                          const int size);
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