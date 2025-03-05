#ifndef BAI_SAMPLING_RULE_H
#define BAI_SAMPLING_RULE_H

typedef struct BAISamplingRule BAISamplingRule;

typedef enum {
  BAI_SAMPLING_RULE_RANDOM,
  BAI_SAMPLING_RULE_UNIFORM,
  BAI_SAMPLING_RULE_TRACK_AND_STOP,
} bai_sampling_rule_t;

BAISamplingRule *bai_sampling_rule_create(bai_sampling_rule_t type, int *N,
                                          int size);
void bai_sampling_rule_destroy(BAISamplingRule *bai_sampling_rule);
int bai_sampling_rule_next_sample(BAISamplingRule *bai_sampling_rule, int astar,
                                  int aalt, double *ξ, double *ϕ2, int *N,
                                  double *S, double *Zs, int size);

#endif