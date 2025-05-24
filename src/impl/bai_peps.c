/*
 * Implements algorithms described in
 *
 * Dealing with Unknown Variances in Best-Arm Identification
 * (https://arxiv.org/pdf/2210.00974)
 *
 * with Julia source code kindly provided by Marc Jourdan.
 */
#include "bai_peps.h"

#include <complex.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>

#include "../util/io_util.h"
#include "../util/math_util.h"

#include "../def/bai_defs.h"

#include "bai_logger.h"

#define BAI_BINARY_SEARCH_MAX_ITER 100

BAIGLRTResults *bai_glrt_results_create(const int K) {
  BAIGLRTResults *glrt_results = malloc_or_die(sizeof(BAIGLRTResults));
  glrt_results->vals = malloc_or_die(K * sizeof(double));
  glrt_results->θs = malloc_or_die(K * sizeof(double));
  return glrt_results;
}

void bai_glrt_results_destroy(BAIGLRTResults *glrt_results) {
  free(glrt_results->vals);
  free(glrt_results->θs);
  free(glrt_results);
}

typedef double (*bai_binary_search_value_func_t)(const double, const void *);

double bai_binary_search(bai_binary_search_value_func_t vf, const void *args,
                         double lo, double hi, const double ϵ,
                         BAILogger __attribute__((unused)) * bai_logger) {
  double flo = vf(lo, args);
  double fhi = vf(hi, args);
  // bai_logger_log_title(bai_logger, "BINARY_SEARCH");
  // bai_logger_log_double(bai_logger, "lo", lo);
  // bai_logger_log_double(bai_logger, "flo", flo);
  // bai_logger_log_double(bai_logger, "hi", hi);
  // bai_logger_log_double(bai_logger, "fhi", fhi);
  if (flo > 0) {
    // bai_logger_log_title(bai_logger, "PLE");
    return lo;
  }
  if (fhi < 0) {
    // bai_logger_log_title(bai_logger, "NHE");
    return hi;
  }

  for (int i = 0; i < (BAI_BINARY_SEARCH_MAX_ITER); i++) {
    double mid = (lo + hi) / 2.0;
    // bai_logger_log_double(bai_logger, "mid", mid);
    if (mid == lo || mid == hi) {
      return mid;
    }

    double fmid = vf(mid, args);
    // bai_logger_log_double(bai_logger, "fmid", fmid);
    if (fmid < -ϵ) {
      lo = mid;
    } else if (fmid > ϵ) {
      hi = mid;
    } else {
      return mid;
    }
  }

  // log_fatal("binary_search did not reach tolerance %0.20f in %d iterations.
  // "
  //           "f(%0.20f) = %0.20f\n"
  //           "f(%0.20f) = %0.20f\n"
  //           "mid would be %0.20f",
  //           ϵ, (BAI_BINARY_SEARCH_MAX_ITER), lo, vf(lo, args), hi, vf(hi,
  //           args), (lo + hi) / 2.0);
  return (lo + hi) / 2.0;
}

double bai_d(const double μ, const double σ2, const double λ) {
  const double diff = μ - λ;
  return 0.5 * (diff * diff) / σ2;
}

double alt_λ(const double μ1, const double σ21, const double w1,
             const double μa, const double σ2a, const double wa,
             BAILogger __attribute__((unused)) * bai_logger) {
  // bai_logger_log_title(bai_logger, "ALT_KV");
  // bai_logger_log_double(bai_logger, "u1", μ1);
  // bai_logger_log_double(bai_logger, "sigma21", σ21);
  // bai_logger_log_double(bai_logger, "w1", w1);
  // bai_logger_log_double(bai_logger, "ua", μa);
  // bai_logger_log_double(bai_logger, "sigma2a", σ2a);
  // bai_logger_log_double(bai_logger, "wa", wa);
  // bai_logger_flush(bai_logger);
  if (w1 == 0) {
    // bai_logger_log_double(bai_logger, "W1_ZERO", μa);
    // bai_logger_flush(bai_logger);
    return μa;
  }
  if (wa == 0 || μ1 == μa) {
    // bai_logger_log_double(bai_logger, "WA_ZERO_OR_U1_EQ_UA", μ1);
    // bai_logger_flush(bai_logger);
    return μ1;
  }
  const double x = wa / w1;
  const double result = (σ2a * μ1 + x * σ21 * μa) / (σ2a + x * σ21);
  // bai_logger_log_double(bai_logger, "x", x);
  // bai_logger_log_double(bai_logger, "result", result);
  // bai_logger_flush(bai_logger);
  return result;
}

void bai_glrt(const int K, const int *w, const double *μ, const double *σ2,
              BAIGLRTResults *glrt_results, BAILogger *bai_logger) {
  bai_logger_log_title(bai_logger, "GLRT");
  bai_logger_log_int(bai_logger, "K", K);

  int astar = 0;
  for (int i = 1; i < K; i++) {
    bai_logger_log_double(bai_logger, "u", μ[i]);
    if (μ[i] > μ[astar]) {
      astar = i;
    }
  }

  bai_logger_log_int(bai_logger, "astar", astar + 1);
  bai_logger_flush(bai_logger);

  double *vals = glrt_results->vals;
  for (int k = 0; k < K; k++) {
    vals[k] = INFINITY;
  }
  double *θs = glrt_results->θs;
  memset(θs, 0, K * sizeof(double));
  for (int a = 0; a < K; a++) {
    if (a == astar) {
      continue;
    }
    θs[a] = alt_λ(μ[astar], σ2[astar], w[astar], μ[a], σ2[a], w[a], bai_logger);
    const double d_astar = bai_d(μ[astar], σ2[astar], θs[a]);
    const double d_a = bai_d(μ[a], σ2[a], θs[a]);
    vals[a] = w[astar] * d_astar + w[a] * d_a;
    bai_logger_log_int(bai_logger, "a", a + 1);
    bai_logger_log_double(bai_logger, "theta", θs[a]);
    bai_logger_log_int(bai_logger, "w_astar", w[astar]);
    bai_logger_log_double(bai_logger, "d_astar", d_astar);
    bai_logger_log_int(bai_logger, "w_a", w[a]);
    bai_logger_log_double(bai_logger, "d_a", d_a);
    bai_logger_log_double(bai_logger, "vals[a]", vals[a]);
    bai_logger_flush(bai_logger);
  }
  int k = 0;
  for (int i = 1; i < K; i++) {
    if (vals[i] < vals[k]) {
      k = i;
    }
  }

  bai_logger_log_int(bai_logger, "k", k + 1);
  bai_logger_log_double(bai_logger, "vals[k]", vals[k]);
  bai_logger_flush(bai_logger);
  glrt_results->k = k;
  glrt_results->astar = astar;
}

typedef struct BAIXBinarySearchArgs {
  double μ1;
  double σ21;
  double μa;
  double σ2a;
  double v;
  BAILogger *bai_logger;
} BAIXBinarySearchArgs;

double bai_X_binary_search_func(const double z, const void *args) {
  const BAIXBinarySearchArgs *xbs_args = (BAIXBinarySearchArgs *)args;
  const double μ1 = xbs_args->μ1;
  const double σ21 = xbs_args->σ21;
  const double μa = xbs_args->μa;
  const double σ2a = xbs_args->σ2a;
  const double v = xbs_args->v;
  const double μz = alt_λ(μ1, σ21, 1 - z, μa, σ2a, z, xbs_args->bai_logger);
  const double bai_d_1_result = bai_d(μ1, σ21, μz);
  const double bai_d_a_result = bai_d(μa, σ2a, μz);
  const double result =
      (1 - z) * bai_d_1_result + z * bai_d_a_result - (1 - z) * v;
  // bai_logger_log_double(xbs_args->bai_logger, "z", z);
  // bai_logger_log_double(xbs_args->bai_logger, "u1", μ1);
  // bai_logger_log_double(xbs_args->bai_logger, "sigma21", σ21);
  // bai_logger_log_double(xbs_args->bai_logger, "ua", μa);
  // bai_logger_log_double(xbs_args->bai_logger, "sigma2a", σ2a);
  // bai_logger_log_double(xbs_args->bai_logger, "v", v);
  // bai_logger_log_double(xbs_args->bai_logger, "uz", μz);
  // bai_logger_log_double(xbs_args->bai_logger, "result", result);
  return result;
}

typedef struct BAIXResults {
  double α_ratio;
  double alt_λ;
} BAIXResults;

void bai_X(const double μ1, const double σ21, const double μa, const double σ2a,
           const double v, BAIXResults *bai_X_results, BAILogger *bai_logger) {
  double upd_a = bai_d(μ1, σ21, μa);
  BAIXBinarySearchArgs xbs_args = {
      .μ1 = μ1,
      .σ21 = σ21,
      .μa = μa,
      .σ2a = σ2a,
      .v = v,
      .bai_logger = bai_logger,
  };
  double α = bai_binary_search(bai_X_binary_search_func, &xbs_args, 0, 1,
                               upd_a * (BAI_EPSILON), bai_logger);
  bai_X_results->α_ratio = α / (1 - α);
  bai_X_results->alt_λ = alt_λ(μ1, σ21, 1 - α, μa, σ2a, α, bai_logger);
  // bai_logger_log_double(bai_logger, "a", α);
  // bai_logger_log_double(bai_logger, "a_ratio", bai_X_results->α_ratio);
  // bai_logger_log_double(bai_logger, "alt", bai_X_results->alt_λ);
  // bai_logger_flush(bai_logger);
}

typedef struct BAIOracleBinarySearchArgs {
  const double *μs;
  const double *σ2s;
  int size;
  int astar;
  BAILogger *bai_logger;
} BAIOracleBinarySearchArgs;

double bai_oracle_binary_search_func(const double z, const void *args) {
  const BAIOracleBinarySearchArgs *obs_args = (BAIOracleBinarySearchArgs *)args;
  const double *μs = obs_args->μs;
  const double *σ2s = obs_args->σ2s;
  const int size = obs_args->size;
  const int astar = obs_args->astar;
  double sum = 0.0;
  BAIXResults bai_X_results;
  for (int k = 0; k < size; ++k) {
    if (k == astar) {
      continue;
    }
    bai_X(μs[astar], σ2s[astar], μs[k], σ2s[k], z, &bai_X_results,
          obs_args->bai_logger);
    const double μx = bai_X_results.alt_λ;
    const double numer = bai_d(μs[astar], σ2s[astar], μx);
    const double denom = bai_d(μs[k], σ2s[k], μx);
    const double result = numer / denom;
    // bai_logger_log_int(obs_args->bai_logger, "k", k + 1);
    // bai_logger_log_double(obs_args->bai_logger, "ux", μx);
    // bai_logger_log_double(obs_args->bai_logger, "numer", numer);
    // bai_logger_log_double(obs_args->bai_logger, "denom", denom);
    // bai_logger_log_double(obs_args->bai_logger, "result", result);
    sum += result;
  }
  return sum - 1.0;
}

BAIOracleResult *bai_oracle_result_create(const int size) {
  BAIOracleResult *result = malloc_or_die(sizeof(BAIOracleResult));
  result->ws_over_Σ = malloc_or_die(size * sizeof(double));
  return result;
}

void bai_oracle_result_destroy(BAIOracleResult *oracle_result) {
  free(oracle_result->ws_over_Σ);
  free(oracle_result);
}

void bai_oracle(const double *μs, const double *σ2s, const int size,
                BAIOracleResult *oracle_result, BAILogger *bai_logger) {
  int astar = 0;
  double μstar = μs[0];
  for (int i = 1; i < size; i++) {
    if (μs[i] > μstar) {
      astar = i;
      μstar = μs[i];
    }
  }

  bool all_equal = true;
  for (int i = 0; i < size; i++) {
    if (μs[i] != μstar) {
      all_equal = false;
      break;
    }
  }

  bai_logger_log_title(bai_logger, "ORACLE");
  bai_logger_log_double(bai_logger, "ustar", μstar);
  bai_logger_log_double_array(bai_logger, "us", μs, size);
  bai_logger_log_int(bai_logger, "size", size);
  bai_logger_flush(bai_logger);

  if (all_equal) {
    for (int i = 0; i < size; i++) {
      oracle_result->ws_over_Σ[i] = 1 / (double)size;
    }
    bai_logger_log_title(bai_logger, "ALL_EQUAL");
    bai_logger_flush(bai_logger);
    return;
  }

  bai_logger_log_int(bai_logger, "astar", astar + 1);
  bai_logger_flush(bai_logger);

  double hi = INFINITY;

  bai_logger_log_double(bai_logger, "us[astar]", μs[astar]);
  bai_logger_log_double(bai_logger, "sigma2s[astar]", σ2s[astar]);
  for (int k = 0; k < size; ++k) {
    if (k == astar) {
      continue;
    }
    double result = bai_d(μs[astar], σ2s[astar], μs[k]);
    bai_logger_log_int(bai_logger, "k", k + 1);
    bai_logger_log_double(bai_logger, "us[k]", μs[k]);
    bai_logger_log_double(bai_logger, "result", result);
    if (result < hi) {
      hi = result;
    }
  }

  bai_logger_log_double(bai_logger, "hi", hi);
  bai_logger_flush(bai_logger);

  BAIOracleBinarySearchArgs obs_args = {
      .astar = astar,
      .size = size,
      .μs = μs,
      .σ2s = σ2s,
      .bai_logger = bai_logger,
  };

  double val = bai_binary_search(bai_oracle_binary_search_func, &obs_args, 0,
                                 hi, BAI_EPSILON, bai_logger);

  bai_logger_log_double(bai_logger, "val", val);
  bai_logger_flush(bai_logger);

  BAIXResults bai_X_results;
  double Σ = 0;
  for (int k = 0; k < size; ++k) {
    if (k == astar) {
      oracle_result->ws_over_Σ[k] = 1.0;
    } else {
      bai_X(μs[astar], σ2s[astar], μs[k], σ2s[k], val, &bai_X_results,
            bai_logger);
      oracle_result->ws_over_Σ[k] = bai_X_results.α_ratio;
    }
    Σ += oracle_result->ws_over_Σ[k];
  }

  bai_logger_log_double_array(bai_logger, "ws", oracle_result->ws_over_Σ, size);
  bai_logger_log_double(bai_logger, "sum", Σ);
  bai_logger_flush(bai_logger);

  for (int k = 0; k < size; ++k) {
    oracle_result->ws_over_Σ[k] /= Σ;
  }
}