#include <stdbool.h>

#include "../ent/random_variable.h"
#include "../util/util.h"

#include "bai_helper.h"
#include "bai_peps.h"
#include "bai_sampling_rule.h"

bool stopping_criterion(int K, double *Zs, BAIThreshold *Sβ, int *N, double *hμ,
                        double *hσ2, int astar) {
  for (int a = 0; a < K; a++) {
    if (a == astar) {
      continue;
    }
    // Original Julia code is:
    // val = is_glr ? Zs[a] : MZs[a];
    // cdt = val > Sβ(N, hμ, hσ2, astar, a);
    // stop = stop && cdt;
    const bool cdt = Zs[a] > bai_invoke_threshold(Sβ, N, hμ, hσ2, astar, a);
    if (!cdt) {
      return false;
    }
  }
  return true;
}

// Assumes random variables are normally distributed.
int bai(bai_sampling_rule_t sr, RandomVariables *rvs, double δ) {
  const int K = rvs_get_num_rvs(rvs);
  BAIThreshold *βs = bai_create_threshold(BAI_THRESHOLD_HT, δ, 2, K, 2, 1.2);

  int *N = calloc_or_die(K, sizeof(int));
  double *S = calloc_or_die(K, sizeof(double));
  double *S2 = calloc_or_die(K, sizeof(double));
  for (int k = 0; k < K; k++) {
    for (int i = 0; i < 2; i++) {
      double _X = rvs_sample(rvs, k);
      S[k] += _X;
      S2[k] += _X * _X;
      N[k] += 1;
    }
  }

  int t = K * 2;
  double *hμ = calloc_or_die(K, sizeof(double));
  double *hσ2 = calloc_or_die(K, sizeof(double));
  // FIXME: probably just inline this
  int astar;
  BAISamplingRule *bai_sampling_rule = bai_sampling_rule_create(sr, N, K);
  BAIThreshold *Sβ = βs;
  BAIGLRTResults *glrt_results = bai_glrt_results_create(K);
  while (true) {
    for (int i = 0; i < K; i++) {
      hμ[i] = S[i] / N[i];
      hσ2[i] = S2[i] / N[i] - hμ[i] * hμ[i];
    }
    bai_glrt(K, N, hμ, hσ2, glrt_results);
    double *Zs = glrt_results->vals;
    int aalt = glrt_results->k;
    astar = glrt_results->astar;
    double *ξ = glrt_results->μ;
    double *ϕ2 = glrt_results->σ2;
    if (stopping_criterion(K, Zs, Sβ, N, hμ, hσ2, astar)) {
      break;
    }
    const int k = bai_sampling_rule_next_sample(bai_sampling_rule, astar, aalt,
                                                ξ, ϕ2, N, S, Zs, K);
    double _X = rvs_sample(rvs, k);
    S[k] += _X;
    S2[k] += _X * _X;
    N[k] += 1;
    t += 1;
  }
  bai_glrt_results_destroy(glrt_results);
  bai_sampling_rule_destroy(bai_sampling_rule);
  bai_destroy_threshold(βs);
  return astar;
}