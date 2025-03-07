#include "bai_peps.h"

#include "../util/log.h"
#include "../util/util.h"

#include "../def/bai_defs.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>

#define BAI_BINARY_SEARCH_MAX_ITER 100

BAIGLRTResults *bai_glrt_results_create(int K) {
  BAIGLRTResults *glrt_results = malloc_or_die(sizeof(BAIGLRTResults));
  glrt_results->vals = malloc_or_die(K * sizeof(double));
  glrt_results->θs = malloc_or_die(K * sizeof(double));
  glrt_results->λ = malloc_or_die(K * sizeof(double));
  glrt_results->ϕ2 = malloc_or_die(K * sizeof(double));
  glrt_results->μ = malloc_or_die(K * sizeof(double));
  glrt_results->σ2 = malloc_or_die(K * sizeof(double));
  return glrt_results;
}

void bai_glrt_results_destroy(BAIGLRTResults *glrt_results) {
  free(glrt_results->vals);
  free(glrt_results->θs);
  free(glrt_results->λ);
  free(glrt_results->ϕ2);
  free(glrt_results->μ);
  free(glrt_results->σ2);
  free(glrt_results);
}

typedef double (*bai_binary_search_value_func_t)(double, void *);

double bai_binary_search(bai_binary_search_value_func_t vf, void *args,
                         double lo, double hi, double ϵ) {
  double flo = vf(lo, args);
  double fhi = vf(hi, args);

  if (flo > 0) {
    log_fatal("f(%f) = %f should be negative at low end", lo, flo);
  }
  if (fhi < 0) {
    log_fatal("f(%f) = %f should be positive at high end", hi, fhi);
  }

  for (int i = 0; i < (BAI_BINARY_SEARCH_MAX_ITER); i++) {
    double mid = (lo + hi) / 2.0;
    if (mid == lo || mid == hi) {
      return mid;
    }

    double fmid = vf(mid, args);

    if (fmid < -ϵ) {
      lo = mid;
    } else if (fmid > ϵ) {
      hi = mid;
    } else {
      return mid;
    }
  }

  // Log fatal error if tolerance is not reached
  log_fatal("binary_search did not reach tolerance %g in %d iterations.\n"
            "f(%f) = %f\n"
            "f(%f) = %f\n"
            "mid would be %f",
            ϵ, (BAI_BINARY_SEARCH_MAX_ITER), lo, vf(lo, args), hi, vf(hi, args),
            (lo + hi) / 2.0);
  // Unreachable but prevents compiler warnings
  return (lo + hi) / 2.0;
}

bool bai_within_epsilon(double x, double y, double ϵ) {
  return fabs(x - y) < ϵ;
}

double alt_λ_KV(double μ1, double σ21, int w1, double μa, double σ2a, int wa) {
  if (w1 == 0) {
    return μa;
  }
  // FIXME: check if this actually does need true equality comparison here
  if (wa == 0 || bai_within_epsilon(μ1, μa, BAI_EPSILON)) {
    return μ1;
  }
  double x = ((double)wa) / w1;
  return (σ2a * μ1 + x * σ21 * μa) / (σ2a + x * σ21);
}

// FIXME: need to check if this is correct
double dGaussian(double μ, double σ2, double λ) {
  const double diff = μ - λ;
  return (diff * diff) / (2 * σ2);
}

void bai_glrt(int K, int *w, double *μ, double *σ2,
              BAIGLRTResults *glrt_results) {
  int astar = 0;
  for (int i = 1; i < K; i++) {
    if (μ[i] > μ[astar]) {
      astar = i;
    }
  }

  double *vals = glrt_results->vals;
  for (int k = 0; k < K; k++) {
    vals[k] = DBL_MAX;
  }
  double *θs = glrt_results->θs;
  // FIXME: probably just use memset here
  for (int k = 0; k < K; k++) {
    θs[k] = 0;
  }
  for (int a = 0; a < K; a++) {
    if (a == astar) {
      continue;
    }
    θs[a] = alt_λ_KV(μ[astar], σ2[astar], w[astar], μ[a], σ2[a], w[a]);
    vals[a] = w[astar] * dGaussian(μ[astar], σ2[astar], θs[a]) +
              w[a] * dGaussian(μ[a], σ2[a], θs[a]);
  }
  int k = 0;
  // Implement argmin
  for (int i = 1; i < K; i++) {
    if (vals[i] < vals[k]) {
      k = i;
    }
  }

  for (int i = 0; i < K; i++) {
    glrt_results->λ[i] = μ[i];
    glrt_results->ϕ2[i] = σ2[i];
    glrt_results->μ[i] = μ[i];
    glrt_results->σ2[i] = σ2[i];
  }
  glrt_results->λ[astar] = θs[k];
  glrt_results->λ[k] = θs[k];
  glrt_results->k = k;
  glrt_results->astar = astar;
}

typedef struct BAIXBinarySearchArgs {
  double μ1;
  double σ21;
  double μa;
  double σ2a;
  double v;
} BAIXBinarySearchArgs;

double bai_X_binary_search_func(double z, void *args) {
  BAIXBinarySearchArgs *xbs_args = (BAIXBinarySearchArgs *)args;
  const double μ1 = xbs_args->μ1;
  const double σ21 = xbs_args->σ21;
  const double μa = xbs_args->μa;
  const double σ2a = xbs_args->σ2a;
  const double v = xbs_args->v;
  const double μz = alt_λ_KV(μ1, σ21, 1 - z, μa, σ2a, z);
  return (1 - z) * dGaussian(μ1, σ21, μz) + z * dGaussian(μa, σ2a, μz) -
         (1 - z) * v;
}

typedef struct BAIXResults {
  double α_ratio;
  double alt_λ_KV;
} BAIXResults;

void bai_X(double μ1, double σ21, double μa, double σ2a, double v,
           BAIXResults *bai_X_results) {
  double upd_a = dGaussian(μ1, σ21, μa);
  BAIXBinarySearchArgs xbs_args = {
      .μ1 = μ1,
      .σ21 = σ21,
      .μa = μa,
      .σ2a = σ2a,
      .v = v,
  };
  double α = bai_binary_search(bai_X_binary_search_func, &xbs_args, 0, 1,
                               upd_a * (BAI_EPSILON));
  bai_X_results->α_ratio = α / (1 - α);
  bai_X_results->alt_λ_KV = alt_λ_KV(μ1, σ21, 1 - α, μa, σ2a, α);
}

typedef struct BAIOracleBinarySearchArgs {
  const double *μs;
  const double *σ2s;
  int size;
  int astar;
} BAIOracleBinarySearchArgs;

double bai_oracle_binary_search_func(double z, void *args) {
  BAIOracleBinarySearchArgs *obs_args = (BAIOracleBinarySearchArgs *)args;
  const double *μs = obs_args->μs;
  const double *σ2s = obs_args->σ2s;
  int size = obs_args->size;
  int astar = obs_args->astar;
  double sum = 0.0;
  BAIXResults bai_X_results;
  for (int k = 0; k < size; ++k) {
    if (k == astar) {
      continue;
    }
    bai_X(μs[astar], σ2s[astar], μs[k], σ2s[k], z, &bai_X_results);
    const double μx = bai_X_results.alt_λ_KV;
    const double num = dGaussian(μs[astar], σ2s[astar], μx);
    const double denom = dGaussian(μs[k], σ2s[k], μx);
    sum += num / denom;
  }
  return sum - 1.0;
}

BAIOracleResult *bai_oracle_result_create(int size) {
  BAIOracleResult *result = malloc_or_die(sizeof(BAIOracleResult));
  result->ws_over_Σ = malloc_or_die(size * sizeof(double));
  return result;
}

void bai_oracle_result_destroy(BAIOracleResult *oracle_result) {
  free(oracle_result->ws_over_Σ);
  free(oracle_result);
}

void bai_oracle(double *μs, double *σ2s, int size,
                BAIOracleResult *oracle_result) {
  int astar = 0;
  double μstar = μs[0];
  bool all_equal = true;
  for (int i = 0; i < size; i++) {
    if (μs[i] > μstar) {
      μstar = μs[i];
      astar = i;
    }
    all_equal = all_equal && bai_within_epsilon(μs[i], μstar, BAI_EPSILON);
  }
  if (all_equal) {
    oracle_result->Σ_over_val = DBL_MAX;
    for (int i = 1; i < size + 1; i++) {
      oracle_result->ws_over_Σ[i] = 1 / (double)size;
    }
    return;
  }

  double hi = DBL_MAX;

  for (int k = 0; k < size; ++k) {
    if (k == astar) {
      continue;
    }
    double result = dGaussian(μs[astar], σ2s[astar], μs[k]);
    if (result < hi) {
      hi = result;
    }
  }

  BAIOracleBinarySearchArgs obs_args;
  obs_args.astar = astar;
  obs_args.size = size;
  obs_args.μs = μs;
  obs_args.σ2s = σ2s;
  double val = bai_binary_search(bai_oracle_binary_search_func, &obs_args, 0,
                                 hi, BAI_EPSILON);
  BAIXResults bai_X_results;
  double Σ = 0;
  for (int k = 0; k < size; ++k) {
    if (k == astar) {
      oracle_result->ws_over_Σ[k] = 1.0;
    } else {
      bai_X(μs[astar], σ2s[astar], μs[k], σ2s[k], val, &bai_X_results);
      oracle_result->ws_over_Σ[k] = bai_X_results.α_ratio;
    }
    Σ += oracle_result->ws_over_Σ[k];
  }
  oracle_result->Σ_over_val = Σ / val;
  for (int k = 0; k < size; ++k) {
    oracle_result->ws_over_Σ[k] /= Σ;
  }
}