#ifndef BAI_HELPER_H
#define BAI_HELPER_H

#include "../def/bai_defs.h"

#include "bai_logger.h"

typedef struct BAIThreshold BAIThreshold;

BAIThreshold *bai_create_threshold(const bai_threshold_t type, const double δ,
                                   const int r, const int s, const double γ);
void bai_destroy_threshold(BAIThreshold *bai_threshold);
double bai_invoke_threshold(const BAIThreshold *bai_threshold, const int *N,
                            const int K, const double *hμ, const double *hσ2,
                            const int astar, const int a,
                            BAILogger *bai_logger);

#endif