#include "bai_helper.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>

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

double barW(double x, int k) { return -lambertw(-exp(-x), k, BAI_EPSILON); }

bool valid_time(HT *ht, int *N) {
  const double δ = ht->δ;
  const double K = ht->K;
  const double s = ht->s;
  const double zetas = ht->zetas;
  const double eta = ht->eta;
  const int cst = 4;
  for (int i = 0; i < K; i++) {
    const double u = 2 * (1 + eta) *
                     (log(cst * (K - 1) * zetas / δ) +
                      s * log(1 + log(N[i]) / log(1 + eta)));
    const double val = exp(1 + lambertw((u - 1) / exp(1), 0, BAI_EPSILON));
    if (N[i] <= val) {
      return false;
    }
  }
  return true;
}

double get_factor_non_KL(HT *ht, int t) {
  const double δ = ht->δ;
  const double K = ht->K;
  const double s = ht->s;
  const double zetas = ht->zetas;
  const double eta = ht->eta;
  const int cst = 4;
  double _val_σ2 = 1 + 2 * (1 + eta) *
                           (log(cst * (K - 1) * zetas / δ) +
                            s * log(1 + log(t) / log(1 + eta))) /
                           t;
  double _val_μ = 1 + 2 * log(cst * (K - 1) * zetas / δ) +
                  2 * s * log(1 + log(t) / (2 * s)) + 2 * s;
  return barW(_val_μ, -1) / (t * barW(_val_σ2, 0) - 1);
}

double HT_threshold(void *data, int *N, double __attribute__((unused)) * hμ,
                    double __attribute__((unused)) * hσ2, int astar, int a) {
  HT *ht = (HT *)data;
  if (!valid_time(ht, N)) {
    return DBL_MAX;
  }
  if (!ht->is_EV_GLR) {
    log_fatal("HT threshold not implemented for non-EV GLR");
  }
  double ratio_a = get_factor_non_KL(ht, N[a]);
  double ratio_astar = get_factor_non_KL(ht, N[astar]);
  return 0.5 * (N[a] * ratio_a + N[astar] * ratio_astar);
}

typedef double (*threshold_func_t)(void *, int *, double *, double *, int, int);

struct BAIThreshold {
  bai_threshold_t type;
  void *data;
  threshold_func_t threshold_func;
};

BAIThreshold *bai_create_threshold(bai_threshold_t type, double δ,
                                   int __attribute__((unused)) r, int K, int s,
                                   double __attribute__((unused)) γ) {
  BAIThreshold *bai_threshold = malloc_or_die(sizeof(BAIThreshold));
  bai_threshold->type = type;
  switch (type) {
  case BAI_THRESHOLD_HT:
    bai_threshold->data = create_HT(δ, K, s, true, false);
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
                            double *hσ2, int astar, int a) {
  return bai_threshold->threshold_func(bai_threshold->data, N, hμ, hσ2, astar,
                                       a);
}