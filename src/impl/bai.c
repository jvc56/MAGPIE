#include <stdbool.h>

#include "xoshiro.h"

#include "bai_helper.h"
#include "bai_peps.h"
#include "bai_rvs.h"
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

// FIXME: σ2s is never used in the original code
// μs and pep are the means and dists respectively
int bai(bai_sampling_rule_t sr, BAIRVS *bairvs, int K, double δ) {
  BAIThreshold *βs = bai_create_threshold(BAI_THRESHOLD_HT, δ, 2, K, 2, 1.2);

  int *N = calloc_or_die(K, sizeof(int));
  double *S = calloc_or_die(K, sizeof(double));
  double *S2 = calloc_or_die(K, sizeof(double));
  for (int k = 0; k < K; k++) {
    for (int i = 0; i < 2; i++) {
      double _X = bai_rvs_sample(bairvs, k);
      S[k] += _X;
      S2[k] += _X * _X;
      N[k] += 1;
    }
  }

  // FIXME: t in original code is set twice:
  // t = sum(N)
  // t += 1
  // figure out if this is indeed unnecessary
  int t = K * 2;
  double *hμ = calloc_or_die(K, sizeof(double));
  double *hσ2 = calloc_or_die(K, sizeof(double));
  // FIXME: probably just inline this
  int astar;
  BAISamplingRule *bai_sampling_rule = bai_sampling_rule_create(sr, N, K);
  BAIThreshold *Sβ = βs;
  GLRTVars *glrt_vars = bai_glrt_vars_create(K);
  while (true) {
    for (int i = 0; i < K; i++) {
      hμ[i] = S[i] / N[i];
      hσ2[i] = S2[i] / N[i] - hμ[i] * hμ[i];
    }
    bai_glrt(K, N, hμ, hσ2, glrt_vars);
    double *Zs = glrt_vars->vals;
    int aalt = glrt_vars->k;
    astar = glrt_vars->astar;
    double *ξ = glrt_vars->μ;
    double *ϕ2 = glrt_vars->σ2;
    if (stopping_criterion(K, Zs, Sβ, N, hμ, hσ2, astar)) {
      break;
    }
    const int k = bai_sampling_rule_next_sample(bai_sampling_rule, astar, aalt,
                                                ξ, ϕ2, N, S, Zs, K);
    double _X = bai_rvs_sample(bairvs, k);
    S[k] += _X;
    S2[k] += _X * _X;
    N[k] += 1;
    t += 1;
  }
  bai_glrt_vars_destroy(glrt_vars);
  bai_sampling_rule_destroy(bai_sampling_rule);
  bai_destroy_threshold(βs);
  return astar;
}