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

double zeta(double s) {
  if (s == 1.0) {
    return INFINITY; // Pole at s=1
  }

  // Check for even negative integers (zeros of zeta)
  if (s <= 0 && s == floor(s) && fmod(-s, 2) == 0) {
    return 0.0;
  }

  // For s < 0 but not at even negative integers, use the functional equation
  if (s < 0) {
    // ζ(s) = 2^s * π^(s-1) * sin(πs/2) * Γ(1-s) * ζ(1-s)
    const double reflection = 1.0 - s;
    const double factor = pow(2.0, s) * pow(M_PI, s - 1.0) *
                          sin(M_PI * s / 2.0) * tgamma(reflection);
    return factor * zeta(reflection);
  }

  // For 0 < s < 1, use the functional equation as above
  if (s > 0 && s < 1) {
    const double reflection = 1.0 - s;
    const double factor = pow(2.0, s) * pow(M_PI, s - 1.0) *
                          sin(M_PI * s / 2.0) * tgamma(reflection);
    return factor * zeta(reflection);
  }

  // For s > 1, use direct summation with Euler-Maclaurin correction
  // Determine how many terms to use based on desired precision
  const int terms = (int)(1000.0 + 500.0 / (s - 1.0));

  // Direct summation
  double sum = 0.0;
  for (int n = 1; n <= terms; n++) {
    sum += 1.0 / pow(n, s);
  }

  // Add Euler-Maclaurin correction terms for faster convergence
  double correction = pow(terms, 1 - s) / (s - 1.0) + 0.5 / pow(terms, s);

  // Add B₂/2! term
  correction += (s / 12.0) / pow(terms, s + 1.0);

  // Add B₄/4! term
  correction -=
      (s * (s + 1.0) * (s + 2.0) * (s + 3.0) / 720.0) / pow(terms, s + 3.0);

  return sum + correction;
}

// Initial approximations for branch 0
double lambertw_branch0(double x) {
  if (x <= 1) {
    double sqeta = sqrt(2.0 + 2.0 * E * x);
    double N2 =
        3.0 * (SQRT2) + 6.0 -
        (((2237.0 + 1457.0 * (SQRT2)) * E - 4108.0 * (SQRT2)-5764.0) * sqeta) /
            ((215.0 + 199.0 * (SQRT2)) * E - 430.0 * (SQRT2)-796.0);
    double N1 = (1.0 - 1.0 / (SQRT2)) * (N2 + (SQRT2));
    return -1.0 + sqeta / (1.0 + N1 * sqeta / (N2 + sqeta));
  } else {
    return log(6.0 * x /
               (5.0 * log(12.0 / 5.0 * (x / log(1.0 + 12.0 * x / 5.0)))));
  }
}

// Initial approximations for branch -1
double lambertw_branch_neg1(double x) {
  const double M1 = 0.3361;
  const double M2 = -0.0042;
  const double M3 = -0.0201;
  double sigma = -1.0 - log(-x);
  return -1.0 - sigma -
         2.0 / M1 *
             (1.0 -
              1.0 / (1.0 + (M1 * sqrt(sigma / 2.0)) /
                               (1.0 + M2 * sigma * exp(M3 * sqrt(sigma)))));
}

// Compute Lambert W function
double lambertw(double x, int k) {
  const double minx = -1.0 / E;
  if (x < minx || (k == -1 && x >= 0)) {
    return NAN;
  }

  double W = (k == 0) ? lambertw_branch0(x) : lambertw_branch_neg1(x);
  double r = fabs(W - log(fabs(x)) + log(fabs(W)));
  int n = 1;

  while (r > BAI_EPSILON && n <= 5) {
    double z = log(x / W) - W;
    double q = 2.0 * (1.0 + W) * (1.0 + W + 2.0 / 3.0 * z);
    double epsilon = z * (q - z) / ((1.0 + W) * (q - 2.0 * z));
    W *= 1.0 + epsilon;

    r = fabs(W - log(fabs(x)) + log(fabs(W)));
    n++;
  }

  return W;
}

double barW(double x, int k) { return -lambertw(-exp(-x), k); }

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
    const double val = exp(1 + lambertw((u - 1) / exp(1), 0));
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