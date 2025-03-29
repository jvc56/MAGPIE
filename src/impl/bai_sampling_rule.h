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

BAISamplingRule *bai_sampling_rule_create(bai_sampling_rule_t type, bool is_EV,
                                          int *N, int size);
void bai_sampling_rule_destroy(BAISamplingRule *bai_sampling_rule);
int bai_sampling_rule_next_sample(BAISamplingRule *bai_sampling_rule, int astar,
                                  int aalt, double *ξ, double *ϕ2, int *N,
                                  double *S, double *Zs, int size,
                                  RandomVariables *rng, BAILogger *bai_logger);

#endif