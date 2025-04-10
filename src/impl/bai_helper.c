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

typedef struct GK16 {
  double δ;
} GK16;

void *create_GK16(const double δ) {
  GK16 *gk16 = malloc_or_die(sizeof(GK16));
  gk16->δ = δ;
  return gk16;
}

void destroy_GK16(GK16 *gk16) { free(gk16); }

double GK16_threshold(const void *data, const int *N, const int K,
                      const double __attribute__((unused)) * hμ,
                      const double __attribute__((unused)) * hσ2,
                      const int __attribute__((unused)) astar,
                      const int __attribute__((unused)) a,
                      BAILogger *bai_logger) {
  const GK16 *gk16 = (GK16 *)data;
  int t = 0;
  for (int i = 0; i < K; i++) {
    t += N[i];
  }
  const double result = log((log((double)t) + 1) / gk16->δ);
  bai_logger_log_title(bai_logger, "GK16");
  bai_logger_log_int(bai_logger, "t", t);
  bai_logger_log_double(bai_logger, "result", result);
  bai_logger_flush(bai_logger);
  return result;
}

typedef struct HT {
  double δ;
  int K;
  int s;
  bool is_EV_GLR;
  bool is_KL;
  double zetas;
  double eta;
} HT;

void *create_HT(const double δ, const int s, const bool is_EV_GLR,
                const bool is_KL) {
  HT *ht = malloc_or_die(sizeof(HT));
  ht->δ = δ;
  ht->s = s;
  ht->is_EV_GLR = is_EV_GLR;
  ht->is_KL = is_KL;
  ht->zetas = zeta(s);
  ht->eta = 1 / log(1 / δ);
  return ht;
}

void destroy_HT(HT *ht) { free(ht); }

double barW(const double x, const int k) { return -lambertw(-exp(-x), k); }

bool valid_time(const HT *ht, const int *N, const int K,
                BAILogger *bai_logger) {
  const double δ = ht->δ;
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
    bai_logger_log_int_array(bai_logger, "N", N, K);
    bai_logger_log_bool(bai_logger, "result", result);
    bai_logger_flush(bai_logger);
    free(u_array);
    free(val_array);
  }
  return result;
}

double get_factor_non_KL(const HT *ht, const int t, const int K,
                         BAILogger *bai_logger) {
  const double δ = ht->δ;
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

double HT_threshold(const void *data, const int *N, const int K,
                    const double __attribute__((unused)) * hμ,
                    const double __attribute__((unused)) * hσ2, const int astar,
                    const int a, BAILogger *bai_logger) {
  const HT *ht = (HT *)data;
  bai_logger_log_title(bai_logger, "HT");
  bai_logger_flush(bai_logger);
  if (!valid_time(ht, N, K, bai_logger)) {
    bai_logger_log_title(bai_logger, "invalid time");
    bai_logger_flush(bai_logger);
    return INFINITY;
  }
  double ratio_a;
  double ratio_astar;
  double result;
  if (ht->is_EV_GLR) {
    ratio_a = get_factor_non_KL(ht, N[a], K, bai_logger);
    ratio_astar = get_factor_non_KL(ht, N[astar], K, bai_logger);
    result = 0.5 * (N[a] * ratio_a + N[astar] * ratio_astar);
  } else {
    ratio_a = get_factor_non_KL(ht, N[a], K, bai_logger);
    ratio_astar = get_factor_non_KL(ht, N[astar], K, bai_logger);
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

typedef double (*threshold_func_t)(const void *, const int *, const int,
                                   const double *, const double *, const int,
                                   const int, BAILogger *);

struct BAIThreshold {
  bai_threshold_t type;
  void *data;
  threshold_func_t threshold_func;
};

BAIThreshold *bai_create_threshold(const bai_threshold_t type, const bool is_EV,
                                   const double δ,
                                   const int __attribute__((unused)) r,
                                   const int s,
                                   const double __attribute__((unused)) γ) {
  BAIThreshold *bai_threshold = malloc_or_die(sizeof(BAIThreshold));
  bai_threshold->type = type;
  switch (type) {
  case BAI_THRESHOLD_GK16:
    bai_threshold->data = create_GK16(δ);
    bai_threshold->threshold_func = GK16_threshold;
    break;
  case BAI_THRESHOLD_HT:
    bai_threshold->data = create_HT(δ, s, is_EV, false);
    bai_threshold->threshold_func = HT_threshold;
    break;
  }
  return bai_threshold;
}

void bai_destroy_threshold(BAIThreshold *bai_threshold) {
  switch (bai_threshold->type) {
  case BAI_THRESHOLD_GK16:
    destroy_GK16((GK16 *)bai_threshold->data);
    break;
  case BAI_THRESHOLD_HT:
    destroy_HT((HT *)bai_threshold->data);
    break;
  }
  free(bai_threshold);
}

double bai_invoke_threshold(const BAIThreshold *bai_threshold, const int *N,
                            const int K, const double *hμ, const double *hσ2,
                            const int astar, const int a,
                            BAILogger *bai_logger) {
  return bai_threshold->threshold_func(bai_threshold->data, N, K, hμ, hσ2,
                                       astar, a, bai_logger);
}