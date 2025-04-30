#ifndef BAI_PEPS_H
#define BAI_PEPS_H

#include "../def/bai_defs.h"

#include "bai_logger.h"

typedef struct BAIOracleResult {
  double *ws_over_Σ;
} BAIOracleResult;

typedef struct BAIGLRTResults {
  double *vals;
  double *θs;
  int k;
  int astar;
} BAIGLRTResults;

BAIGLRTResults *bai_glrt_results_create(int K);
void bai_glrt_results_destroy(BAIGLRTResults *glrt_results);
void bai_glrt(const int K, const int *w, const double *μ, const double *σ2,
              BAIGLRTResults *glrt_results, BAILogger *bai_logger);
BAIOracleResult *bai_oracle_result_create(int size);
void bai_oracle_result_destroy(BAIOracleResult *result);
void bai_oracle(const double *μs, const double *σ2s, const int size,
                BAIOracleResult *oracle_result, BAILogger *bai_logger);

#endif