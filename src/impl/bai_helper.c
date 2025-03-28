#include "bai_helper.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>

#include "../ent/bai_logger.h"

#include "../util/log.h"
#include "../util/math_util.h"
#include "../util/util.h"

#include "../def/bai_defs.h"
#include "../def/math_util_defs.h"

typedef struct HT {
  double δ;
  int K;
  int s;
  bool is_EV_GLR;
  bool is_KL;
  double zetas;
  double eta;
} HT;

void *create_HT(double δ, int K, int s, bool is_EV_GLR, bool is_KL) {
  HT *ht = malloc_or_die(sizeof(HT));
  ht->δ = δ;
  ht->K = K;
  ht->s = s;
  ht->is_EV_GLR = is_EV_GLR;
  ht->is_KL = is_KL;
  ht->zetas = zeta(s);
  ht->eta = 1 / log(1 / δ);
  return ht;
}

void destroy_HT(HT *ht) { free(ht); }

double barW(double x, int k) { return -lambertw(-exp(-x), k); }

bool valid_time(HT *ht, int *N, BAILogger *bai_logger) {
  const double δ = ht->δ;
  const int K = ht->K;
  const double s = ht->s;
  const double zetas = ht->zetas;
  const double eta = ht->eta;
  const int cst = 4;
  int sum = 0;
  double *u_array = NULL;
  double *val_array = NULL;
  if (bai_logger) {
    u_array = malloc_or_die(K * sizeof(double));
    val_array = malloc_or_die(K * sizeof(double));
  }
  for (int i = 0; i < K; i++) {
    const double u = 2 * (1 + eta) *
                     (log(cst * (K - 1) * zetas / δ) +
                      s * log(1 + log(N[i]) / log(1 + eta)));
    const double val = exp(1 + lambertw((u - 1) / exp(1), 0));
    if (bai_logger) {
      u_array[i] = u;
      val_array[i] = val;
    }
    if (N[i] > val) {
      sum++;
    }
  }
  const bool result = sum == K;
  if (bai_logger) {
    bai_logger_log_title(bai_logger, "VALID_TIME");
    bai_logger_log_double(bai_logger, "delta", δ);
    bai_logger_log_int(bai_logger, "K", K);
    bai_logger_log_double(bai_logger, "s", s);
    bai_logger_log_double(bai_logger, "zetas", zetas);
    bai_logger_log_double(bai_logger, "eta", eta);
    bai_logger_log_int(bai_logger, "cst", cst);
    bai_logger_log_double_array(bai_logger, "u", u_array, K);
    bai_logger_log_double_array(bai_logger, "vals", val_array, K);
    bai_logger_log_bool(bai_logger, "result", result);
    bai_logger_flush(bai_logger);
    free(u_array);
    free(val_array);
  }
  return result;
}

double get_factor_non_KL(HT *ht, int t, BAILogger *bai_logger) {
  const double δ = ht->δ;
  const int K = ht->K;
  const double s = ht->s;
  const double zetas = ht->zetas;
  const double eta = ht->eta;
  const int cst = 4;
  const double _val_σ2 = 1 + 2 * (1 + eta) *
                                 (log(cst * (K - 1) * zetas / δ) +
                                  s * log(1 + log(t) / log(1 + eta))) /
                                 t;
  const double _val_μ = 1 + 2 * log(cst * (K - 1) * zetas / δ) +
                        2 * s * log(1 + log(t) / (2 * s)) + 2 * s;
  const double numerator = barW(_val_μ, -1);
  const double denominator = (t * barW(_val_σ2, 0) - 1);
  const double ratio = numerator / denominator;
  bai_logger_log_title(bai_logger, "GET_FACTOR");
  bai_logger_log_int(bai_logger, "t", t);
  bai_logger_log_double(bai_logger, "delta", δ);
  bai_logger_log_int(bai_logger, "K", K);
  bai_logger_log_double(bai_logger, "s", s);
  bai_logger_log_double(bai_logger, "zetas", zetas);
  bai_logger_log_double(bai_logger, "eta", eta);
  bai_logger_log_int(bai_logger, "cst", cst);
  bai_logger_log_double(bai_logger, "_val_sigma2", _val_σ2);
  bai_logger_log_double(bai_logger, "_val_mu", _val_μ);
  bai_logger_log_double(bai_logger, "numerator", numerator);
  bai_logger_log_double(bai_logger, "denominator", denominator);
  bai_logger_log_double(bai_logger, "ratio", ratio);
  bai_logger_flush(bai_logger);
  return ratio;
}

double HT_threshold(void *data, int *N, double __attribute__((unused)) * hμ,
                    double __attribute__((unused)) * hσ2, int astar, int a,
                    BAILogger *bai_logger) {
  HT *ht = (HT *)data;
  bai_logger_log_title(bai_logger, "HT");
  bai_logger_flush(bai_logger);
  if (!valid_time(ht, N, bai_logger)) {
    bai_logger_log_title(bai_logger, "invalid time");
    bai_logger_flush(bai_logger);
    return INFINITY;
  }
  double ratio_a;
  double ratio_astar;
  double result;
  if (ht->is_EV_GLR) {
    ratio_a = get_factor_non_KL(ht, N[a], bai_logger);
    ratio_astar = get_factor_non_KL(ht, N[astar], bai_logger);
    result = 0.5 * (N[a] * ratio_a + N[astar] * ratio_astar);
  } else {
    ratio_a = get_factor_non_KL(ht, N[a], bai_logger);
    ratio_astar = get_factor_non_KL(ht, N[astar], bai_logger);
    result = 0.5 * (N[a] * log(1 + ratio_a) + N[astar] * log(1 + ratio_astar));
  }
  bai_logger_log_double(bai_logger, "ratio_a", ratio_a);
  bai_logger_log_double(bai_logger, "ratio_astar", ratio_astar);
  bai_logger_log_int(bai_logger, "N[a]", N[a]);
  bai_logger_log_int(bai_logger, "N[astar]", N[astar]);
  bai_logger_log_double(bai_logger, "result", result);
  bai_logger_flush(bai_logger);
  return result;
}

typedef double (*threshold_func_t)(void *, int *, double *, double *, int, int,
                                   BAILogger *);

struct BAIThreshold {
  bai_threshold_t type;
  void *data;
  threshold_func_t threshold_func;
};

BAIThreshold *bai_create_threshold(bai_threshold_t type, bool is_EV, double δ,
                                   int __attribute__((unused)) r, int K, int s,
                                   double __attribute__((unused)) γ) {
  BAIThreshold *bai_threshold = malloc_or_die(sizeof(BAIThreshold));
  bai_threshold->type = type;
  switch (type) {
  case BAI_THRESHOLD_HT:
    bai_threshold->data = create_HT(δ, K, s, is_EV, false);
    bai_threshold->threshold_func = HT_threshold;
    break;
  }
  return bai_threshold;
}

void bai_destroy_threshold(BAIThreshold *bai_threshold) {
  switch (bai_threshold->type) {
  case BAI_THRESHOLD_HT:
    destroy_HT((HT *)bai_threshold->data);
    break;
  }
  free(bai_threshold);
}

double bai_invoke_threshold(BAIThreshold *bai_threshold, int *N, double *hμ,
                            double *hσ2, int astar, int a,
                            BAILogger *bai_logger) {
  return bai_threshold->threshold_func(bai_threshold->data, N, hμ, hσ2, astar,
                                       a, bai_logger);
}