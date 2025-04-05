#include <stdbool.h>

#include "../ent/bai_logger.h"
#include "../ent/random_variable.h"

#include "../util/util.h"

#include "bai_helper.h"
#include "bai_peps.h"
#include "bai_sampling_rule.h"

#define EPIGON_PENALTY -1e30

bool stopping_criterion(int K, double *Zs, BAIThreshold *Sβ, int *N, double *hμ,
                        double *hσ2, int astar, BAILogger *bai_logger) {
  for (int a = 0; a < K; a++) {
    if (a == astar) {
      continue;
    }
    // Original Julia code is:
    // val = is_glr ? Zs[a] : MZs[a];
    // cdt = val > Sβ(N, hμ, hσ2, astar, a);
    // stop = stop && cdt;
    const double thres =
        bai_invoke_threshold(Sβ, N, hμ, hσ2, astar, a, bai_logger);
    const bool cdt = Zs[a] > thres;
    bai_logger_log_title(bai_logger, "STOPPING_CRITERION");
    bai_logger_log_int(bai_logger, "a", a + 1);
    bai_logger_log_double(bai_logger, "val", Zs[a]);
    bai_logger_log_double(bai_logger, "thres", thres);
    bai_logger_flush(bai_logger);
    if (!cdt) {
      return false;
    }
  }
  return true;
}

// Assumes rvs are normally distributed.
// Assumes rng is uniformly distributed.
int bai(bai_sampling_rule_t sr, bool is_EV, bai_threshold_t thres,
        RandomVariables *rvs, double δ, RandomVariables *rng, int sample_limit,
        int similar_play_min_iter_for_eval, BAILogger *bai_logger) {
  const int K = rvs_get_num_rvs(rvs);
  BAIThreshold *βs = bai_create_threshold(thres, is_EV, δ, 2, K, 2, 1.2);

  int *N = calloc_or_die(K, sizeof(int));
  double *S = calloc_or_die(K, sizeof(double));
  double *S2 = calloc_or_die(K, sizeof(double));
  double *hμ = calloc_or_die(K, sizeof(double));
  double *hσ2 = calloc_or_die(K, sizeof(double));
  bool *is_similarity_evaluated = calloc_or_die(K, sizeof(bool));
  BAISamplingRule *bai_sampling_rule =
      bai_sampling_rule_create(sr, is_EV, N, K);
  BAIThreshold *Sβ = βs;
  BAIGLRTResults *glrt_results = bai_glrt_results_create(K);
  int t = 0;
  for (int k = 0; k < K; k++) {
    for (int i = 0; i < 2; i++) {
      double _X = rvs_sample(rvs, k, bai_logger);
      S[k] += _X;
      S2[k] += _X * _X;
      N[k] += 1;
      t++;
    }
  }

  int astar;
  int num_epigons = 0;
  while (true) {
    for (int i = 0; i < K; i++) {
      hμ[i] = S[i] / N[i];
      hσ2[i] = S2[i] / N[i] - hμ[i] * hμ[i];
    }
    bai_logger_log_int(bai_logger, "t", t);
    bai_glrt(K, N, hμ, hσ2, is_EV, glrt_results, bai_logger);
    double *Zs = glrt_results->vals;
    int aalt = glrt_results->k;
    astar = glrt_results->astar;
    double *ξ = glrt_results->μ;
    double *ϕ2 = glrt_results->σ2;

    bai_logger_log_title(bai_logger, "GLRT_RETURN_VALUES");
    bai_logger_log_double_array(bai_logger, "Zs", Zs, K);
    bai_logger_log_int(bai_logger, "aalt", aalt + 1);
    bai_logger_log_int(bai_logger, "astar", astar + 1);
    bai_logger_log_double_array(bai_logger, "ksi", ξ, K);
    bai_logger_log_double_array(bai_logger, "phi2", ϕ2, K);
    bai_logger_flush(bai_logger);

    if (stopping_criterion(K, Zs, Sβ, N, hμ, hσ2, astar, bai_logger)) {
      break;
    }
    const int k = bai_sampling_rule_next_sample(
        bai_sampling_rule, astar, aalt, ξ, ϕ2, N, S, Zs, K, rng, bai_logger);
    double _X;
    if (rvs_is_epigon(rvs, k)) {
      _X = EPIGON_PENALTY;
    } else {
      _X = rvs_sample(rvs, k, bai_logger);
    }
    S[k] += _X;
    S2[k] += _X * _X;
    N[k] += 1;
    t += 1;
    if (t >= sample_limit) {
      bai_logger_log_title(bai_logger, "REACHED_SAMPLE_LIMIT");
      break;
    }
    if (similar_play_min_iter_for_eval > 0 &&
        N[astar] >= similar_play_min_iter_for_eval &&
        !is_similarity_evaluated[astar]) {
      for (int i = 0; i < K; i++) {
        if (i == astar || rvs_is_epigon(rvs, i)) {
          continue;
        }
        if (rvs_mark_as_epigon_if_similar(rvs, astar, i)) {
          num_epigons++;
        }
      }
      if (num_epigons == K - 1) {
        break;
      }
      is_similarity_evaluated[astar] = true;
    }
  }
  bai_glrt_results_destroy(glrt_results);
  bai_sampling_rule_destroy(bai_sampling_rule);
  free(hσ2);
  free(hμ);
  free(S2);
  free(S);
  free(N);
  free(is_similarity_evaluated);
  bai_destroy_threshold(βs);
  return astar;
}