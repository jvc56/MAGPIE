#include "bai_peps.h"

#include "../util/log.h"
#include "../util/math_util.h"
#include "../util/util.h"

#include "../def/bai_defs.h"

#include "../ent/bai_logger.h"

#include <complex.h>
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
                         double lo, double hi, double ϵ,
                         BAILogger *bai_logger) {
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
    bai_logger_log_double(bai_logger, "mid", mid);
    if (mid == lo || mid == hi) {
      return mid;
    }

    double fmid = vf(mid, args);
    bai_logger_log_double(bai_logger, "fmid", fmid);
    if (fmid < -ϵ) {
      lo = mid;
    } else if (fmid > ϵ) {
      hi = mid;
    } else {
      return mid;
    }
  }

  // Log fatal error if tolerance is not reached
  log_fatal("binary_search did not reach tolerance %0.20f in %d iterations.\n"
            "f(%0.20f) = %0.20f\n"
            "f(%0.20f) = %0.20f\n"
            "mid would be %0.20f",
            ϵ, (BAI_BINARY_SEARCH_MAX_ITER), lo, vf(lo, args), hi, vf(hi, args),
            (lo + hi) / 2.0);
  // Unreachable but prevents compiler warnings
  return (lo + hi) / 2.0;
}

bool bai_within_epsilon(double x, double y, double ϵ) {
  return fabs(x - y) < ϵ;
}

double bai_dUV(double μ, double σ2, double λ) {
  const double diff = μ - λ;
  return 0.5 * log(1 + (diff * diff) / σ2);
}

double bai_dKV(double μ, double σ2, double λ) {
  const double diff = μ - λ;
  return 0.5 * (diff * diff) / σ2;
}

double bai_d(double μ, double σ2, double λ, bool known_var) {
  if (known_var) {
    return bai_dKV(μ, σ2, λ);
  } else {
    return bai_dUV(μ, σ2, λ);
  }
}

double alt_λ_KV(double μ1, double σ21, double w1, double μa, double σ2a,
                double wa, BAILogger *bai_logger) {
  bai_logger_log_title(bai_logger, "ALT_KV");
  if (w1 == 0) {
    bai_logger_log_double(bai_logger, "ua", μa);
    return μa;
  }
  if (wa == 0 || μ1 == μa) {
    bai_logger_log_double(bai_logger, "u1", μ1);
    return μ1;
  }
  const double x = wa / w1;
  const double result = (σ2a * μ1 + x * σ21 * μa) / (σ2a + x * σ21);
  bai_logger_log_double(bai_logger, "x", x);
  bai_logger_log_double(bai_logger, "result", result);
  return result;
}

double alt_λ_UV(double μ1, double σ21, double w1, double μa, double σ2a,
                double wa, BAILogger *bai_logger) {
  bai_logger_log_title(bai_logger, "ALT_UV");
  if (w1 == 0) {
    bai_logger_log_double(bai_logger, "ua", μa);
    return μa;
  }
  if (wa == 0 || μ1 == μa) {
    bai_logger_log_double(bai_logger, "u1", μ1);
    return μ1;
  }
  const double x = wa / w1;
  const double α2 = μa + μ1 + (μa + x * μ1) / (1 + x);
  const double α1 =
      (σ2a + x * σ21) / (1 + x) + μa * μ1 + (μa + μ1) * (μa + x * μ1) / (1 + x);
  const double α0 = (μ1 * (μa * μa + σ2a) + μa * (μ1 * μ1 + σ21) * x) / (1 + x);

  bai_logger_log_double(bai_logger, "x", x);
  bai_logger_log_double(bai_logger, "alpha2", α2);
  bai_logger_log_double(bai_logger, "alpha1", α1);
  bai_logger_log_double(bai_logger, "alpha0", α0);

  complex double roots[3];
  const bool cubic_root_success = cubic_roots(1, -α2, α1, -α0, roots);
  if (!cubic_root_success) {
    log_fatal("cubic solver failed for inputs: %.15f, %.15f, %.15f, %.15f", 1,
              -α2, α1, -α0);
  }
  int num_valid_roots = 0;
  double valid_roots[3];
  for (int i = 0; i < 3; i++) {
    if (fabs(cimag(roots[i])) < 1e-10) {
      const double r = creal(roots[i]);
      if (r - μa >= -1e-10 && r - μ1 <= 1e-10) {
        valid_roots[num_valid_roots] = r;
        num_valid_roots++;
      }
    }
  }

  bai_logger_log_int(bai_logger, "num_valid_roots", num_valid_roots);

  if (num_valid_roots == 0) {
    log_fatal("Solver couldn't find a valid real roots in [%.15f, %.15f].", μa,
              μ1);
  }
  if (num_valid_roots == 1) {
    bai_logger_log_double(bai_logger, "valid_root[1]", valid_roots[0]);
    return valid_roots[0];
  }
  double v =
      bai_dUV(μ1, σ21, valid_roots[0]) + x * bai_dUV(μa, σ2a, valid_roots[0]);
  int id = 0;
  for (int i = 1; i < num_valid_roots; i++) {
    double _v =
        bai_dUV(μ1, σ21, valid_roots[i]) + x * bai_dUV(μa, σ2a, valid_roots[i]);
    if (_v < v) {
      id = i;
      v = _v;
    }
  }
  bai_logger_log_double(bai_logger, "valid_root[id]", valid_roots[id]);
  return valid_roots[id];
}

double alt_λ(double μ1, double σ21, double w1, double μa, double σ2a, double wa,
             bool known_var, BAILogger *bai_logger) {
  if (known_var) {
    return alt_λ_KV(μ1, σ21, w1, μa, σ2a, wa, bai_logger);
  } else {
    return alt_λ_UV(μ1, σ21, w1, μa, σ2a, wa, bai_logger);
  }
}

void bai_glrt(int K, int *w, double *μ, double *σ2, bool known_var,
              BAIGLRTResults *glrt_results, BAILogger *bai_logger) {
  int astar = 0;
  for (int i = 1; i < K; i++) {
    if (μ[i] > μ[astar]) {
      astar = i;
    }
  }

  bai_logger_log_title(bai_logger, "GLRT");
  bai_logger_log_int(bai_logger, "K", K);
  bai_logger_log_int(bai_logger, "astar", astar + 1);
  bai_logger_flush(bai_logger);

  double *vals = glrt_results->vals;
  for (int k = 0; k < K; k++) {
    vals[k] = INFINITY;
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
    θs[a] = alt_λ(μ[astar], σ2[astar], w[astar], μ[a], σ2[a], w[a], known_var,
                  bai_logger);
    vals[a] = w[astar] * bai_d(μ[astar], σ2[astar], θs[a], known_var) +
              w[a] * bai_d(μ[a], σ2[a], θs[a], known_var);
    bai_logger_log_int(bai_logger, "arm", a + 1);
    bai_logger_log_double(bai_logger, "theta", θs[a]);
    bai_logger_log_double(bai_logger, "val", vals[a]);
    bai_logger_flush(bai_logger);
  }
  int k = 0;
  // Implement argmin
  for (int i = 1; i < K; i++) {
    if (vals[i] < vals[k]) {
      k = i;
    }
  }

  bai_logger_log_int(bai_logger, "k", k + 1);
  bai_logger_log_double(bai_logger, "vals[k]", vals[k]);
  bai_logger_flush(bai_logger);

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

  bai_logger_log_double_array(bai_logger, "lambda", glrt_results->λ, K);
  bai_logger_log_double_array(bai_logger, "phi2", glrt_results->ϕ2, K);
  bai_logger_flush(bai_logger);
}

typedef struct BAIXBinarySearchArgs {
  double μ1;
  double σ21;
  double μa;
  double σ2a;
  double v;
  bool known_var;
  BAILogger *bai_logger;
} BAIXBinarySearchArgs;

double bai_X_binary_search_func(double z, void *args) {
  BAIXBinarySearchArgs *xbs_args = (BAIXBinarySearchArgs *)args;
  const double μ1 = xbs_args->μ1;
  const double σ21 = xbs_args->σ21;
  const double μa = xbs_args->μa;
  const double σ2a = xbs_args->σ2a;
  const double v = xbs_args->v;
  const bool known_var = xbs_args->known_var;
  const double μz =
      alt_λ(μ1, σ21, 1 - z, μa, σ2a, z, known_var, xbs_args->bai_logger);
  const double result = (1 - z) * bai_d(μ1, σ21, μz, known_var) +
                        z * bai_d(μa, σ2a, μz, known_var) - (1 - z) * v;
  bai_logger_log_double(xbs_args->bai_logger, "z", z);
  bai_logger_log_double(xbs_args->bai_logger, "u1", μ1);
  bai_logger_log_double(xbs_args->bai_logger, "sigma21", σ21);
  bai_logger_log_double(xbs_args->bai_logger, "ua", μa);
  bai_logger_log_double(xbs_args->bai_logger, "sigma2a", σ2a);
  bai_logger_log_double(xbs_args->bai_logger, "v", v);
  bai_logger_log_bool(xbs_args->bai_logger, "kv", true);
  bai_logger_log_double(xbs_args->bai_logger, "uz", μz);
  bai_logger_log_double(xbs_args->bai_logger, "result", result);
  return result;
}

typedef struct BAIXResults {
  double α_ratio;
  double alt_λ;
} BAIXResults;

void bai_X(double μ1, double σ21, double μa, double σ2a, double v,
           bool known_var, BAIXResults *bai_X_results, BAILogger *bai_logger) {
  double upd_a = bai_d(μ1, σ21, μa, known_var);
  BAIXBinarySearchArgs xbs_args = {
      .μ1 = μ1,
      .σ21 = σ21,
      .μa = μa,
      .σ2a = σ2a,
      .v = v,
      .known_var = known_var,
      .bai_logger = bai_logger,
  };
  double α = bai_binary_search(bai_X_binary_search_func, &xbs_args, 0, 1,
                               upd_a * (BAI_EPSILON), bai_logger);
  bai_X_results->α_ratio = α / (1 - α);
  bai_X_results->alt_λ =
      alt_λ(μ1, σ21, 1 - α, μa, σ2a, α, known_var, bai_logger);
  bai_logger_log_double(bai_logger, "a", α);
  bai_logger_log_double(bai_logger, "a_ratio", bai_X_results->α_ratio);
  bai_logger_log_double(bai_logger, "alt", bai_X_results->alt_λ);
  bai_logger_flush(bai_logger);
}

typedef struct BAIOracleBinarySearchArgs {
  const double *μs;
  const double *σ2s;
  int size;
  int astar;
  bool known_var;
  BAILogger *bai_logger;
} BAIOracleBinarySearchArgs;

double bai_oracle_binary_search_func(double z, void *args) {
  BAIOracleBinarySearchArgs *obs_args = (BAIOracleBinarySearchArgs *)args;
  const double *μs = obs_args->μs;
  const double *σ2s = obs_args->σ2s;
  const int size = obs_args->size;
  const int astar = obs_args->astar;
  const bool known_var = obs_args->known_var;
  double sum = 0.0;
  BAIXResults bai_X_results;
  for (int k = 0; k < size; ++k) {
    if (k == astar) {
      continue;
    }
    bai_X(μs[astar], σ2s[astar], μs[k], σ2s[k], z, known_var, &bai_X_results,
          obs_args->bai_logger);
    const double μx = bai_X_results.alt_λ;
    const double numer = bai_d(μs[astar], σ2s[astar], μx, known_var);
    const double denom = bai_d(μs[k], σ2s[k], μx, known_var);
    const double result = numer / denom;
    bai_logger_log_int(obs_args->bai_logger, "k", k + 1);
    bai_logger_log_double(obs_args->bai_logger, "ux", μx);
    bai_logger_log_double(obs_args->bai_logger, "numer", numer);
    bai_logger_log_double(obs_args->bai_logger, "denom", denom);
    bai_logger_log_double(obs_args->bai_logger, "result", result);
    sum += result;
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

void bai_oracle(double *μs, double *σ2s, int size, bool known_var,
                BAIOracleResult *oracle_result, BAILogger *bai_logger) {
  int astar = 0;
  double μstar = μs[0];
  for (int i = 1; i < size; i++) {
    if (μs[i] > μstar) {
      μstar = μs[i];
      astar = i;
    }
  }

  bool all_equal = true;
  for (int i = 0; i < size; i++) {
    if (!bai_within_epsilon(μs[i], μstar, BAI_EPSILON)) {
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
    oracle_result->Σ_over_val = INFINITY;
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

  for (int k = 0; k < size; ++k) {
    if (k == astar) {
      continue;
    }
    double result = bai_d(μs[astar], σ2s[astar], μs[k], known_var);
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
      .known_var = known_var,
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
      bai_X(μs[astar], σ2s[astar], μs[k], σ2s[k], val, known_var,
            &bai_X_results, bai_logger);
      oracle_result->ws_over_Σ[k] = bai_X_results.α_ratio;
    }
    Σ += oracle_result->ws_over_Σ[k];
  }

  bai_logger_log_double_array(bai_logger, "ws", oracle_result->ws_over_Σ, size);
  bai_logger_log_double(bai_logger, "sum", Σ);
  bai_logger_log_double(bai_logger, "sum/val", Σ / val);
  bai_logger_flush(bai_logger);

  oracle_result->Σ_over_val = Σ / val;
  for (int k = 0; k < size; ++k) {
    oracle_result->ws_over_Σ[k] /= Σ;
  }
}