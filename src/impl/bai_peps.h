#ifndef BAI_PEPS_H
#define BAI_PEPS_H

typedef struct BAIOracleResult {
  double Σ_over_val;
  double *ws_over_Σ;
} BAIOracleResult;

typedef struct BAIGLRTResults {
  double *vals;
  double *θs;
  int k;
  double *λ;
  double *ϕ2;
  // FIXME: determine if these are needed
  int astar;
  double *μ;
  double *σ2;
} BAIGLRTResults;

BAIGLRTResults *bai_glrt_results_create(int K);
void bai_glrt_results_destroy(BAIGLRTResults *glrt_results);
void bai_glrt(int K, int *w, double *μ, double *σ2,
              BAIGLRTResults *glrt_results);
BAIOracleResult *bai_oracle_result_create(int size);
void bai_oracle_result_destroy(BAIOracleResult *result);
void bai_oracle(double *μs, double *σ2s, int size,
                BAIOracleResult *oracle_result);

#endif