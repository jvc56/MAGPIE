#ifndef BAI_HELPER_H
#define BAI_HELPER_H

#include "../ent/bai_logger.h"

typedef struct BAIThreshold BAIThreshold;

typedef enum {
  BAI_THRESHOLD_HT,
} bai_threshold_t;

BAIThreshold *bai_create_threshold(bai_threshold_t type, double δ, int r, int K,
                                   int s, double γ);
void bai_destroy_threshold(BAIThreshold *bai_threshold);
double bai_invoke_threshold(BAIThreshold *bai_threshold, int *N, double *hμ,
                            double *hσ2, int astar, int a,
                            BAILogger *bai_logger);

#endif