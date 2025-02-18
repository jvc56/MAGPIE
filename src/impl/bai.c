#include "bai.h"

#include <float.h>
#include <stdbool.h>

#include "xoshiro.h"

#define BAI_EPSILON 0.0000001

double bai_within_epsilon(double x, double y) {
  return abs(x - y) < BAI_EPSILON;
}

double alt_λ_KV(double μ1, double σ21, int w1, double μa, double σ2a, int wa) {
  if (w1 == 0) {
    return μa;
  }
  if (wa == 0 || bai_within_epsilon(μ1, μa)) {
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

typedef struct GLRTVars {
  double *vals;
  double *θs;
  int k;
  double *λ;
  double *ϕ2;
  // FIXME: determine if these are needed
  int astar;
  double *μ;
  double *σ2;
} GLRTVars;

GLRTVars *GLRTVars_create(int K) {
  GLRTVars *glrt_vars = malloc_or_die(sizeof(GLRTVars));
  glrt_vars->vals = malloc_or_die(K * sizeof(double));
  glrt_vars->θs = malloc_or_die(K * sizeof(double));
  glrt_vars->λ = malloc_or_die(K * sizeof(double));
  glrt_vars->ϕ2 = malloc_or_die(K * sizeof(double));
  glrt_vars->μ = malloc_or_die(K * sizeof(double));
  glrt_vars->σ2 = malloc_or_die(K * sizeof(double));
  return glrt_vars;
}

void GLRTVars_destroy(GLRTVars *glrt_vars) {
  free(glrt_vars->vals);
  free(glrt_vars->θs);
  free(glrt_vars->λ);
  free(glrt_vars->ϕ2);
  free(glrt_vars->μ);
  free(glrt_vars->σ2);
  free(glrt_vars);
}

void glrt(int K, int *w, double *μ, double *σ2, GLRTVars *glrt_vars) {
  int astar = 0;
  for (int i = 1; i < K; i++) {
    if (μ[i] > μ[astar]) {
      astar = i;
    }
  }

  double *vals = glrt_vars->vals;
  for (int k = 0; k < K; k++) {
    vals[k] = DBL_MAX;
  }
  double *θs = glrt_vars->θs;
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
    glrt_vars->λ[i] = μ[i];
    glrt_vars->ϕ2[i] = σ2[i];
    glrt_vars->μ[i] = μ[i];
    glrt_vars->σ2[i] = σ2[i];
  }
  glrt_vars->λ[astar] = θs[k];
  glrt_vars->λ[k] = θs[k];
  glrt_vars->k = k;
  glrt_vars->astar = astar;
}

bool stopping_condition(int K, double *Zs, Sβ, int *N, double *hμ, double *hσ2,
                        int astar) {
  for (int a = 0; a < K; a++) {
    if (a == astar) {
      continue;
    }
    // Original Julia code is:
    // val = is_glr ? Zs[a] : MZs[a];
    // cdt = val > Sβ(N, hμ, hσ2, astar, a);
    // stop = stop && cdt;
    const bool cdt = Zs[a] > Sβ(N, hμ, hσ2, astar, a);
    if (!cdt) {
      return false;
    }
  }
  return true;
}

typedef struct BAIThreshold {
  void *instance;
} BAIThreshold;

typedef struct HT {
  double δ;
  int K;
  int s;
  bool is_EV_GLR;
  bool is_KL;
  double zetas;
  eta;
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

void get_threshold(bai_threshold_t type, double δ, int r, int K, int s,
                   double γ, BAIThreshold *bai_threshold) {
  switch (type) {
  case BAI_THRESHOLD_HT:
    bai_threshold->instance = create_HT(δ, K, s, true, false);
    break;
  }
}

// # Hyperbox Thresholds
// # The ones for EV-GLR stopping rule have is_GLR=true
// # The ones for GLR stopping rule based on KL have is_KL=true
// struct HT
//     δ;
//     K; # dimension
//     s; # factor s
//     is_GLR; # EV-GLR
//     is_KL; # KL instead of log
//     zetas;
//     eta;
//     HT(δ, K, s, is_GLR, is_KL) = new(δ, K, s, is_GLR, is_KL, zeta(s),
//     1/log(1/δ));
// end

// # Get thresholds
// βs = get_threshold(rsp.threshold, δs, 2, length(μs), 2, 1.2);

// elseif threshold == "HT-EV"
// βs = HT.(δs, K, s, true, false);
// FIXME: σ2s is never used in the original code
// μs and pep are the means and dists respectively
int bai(XoshiroPRNG *prng, int K, BAIRV **bairvs, double δ) {
  // FIXME: get threshold
  BAIThreshold βs;
  bai_get_threshold(BAI_THRESHOLD_HT, δ, 2, K, 2, 1.2, &βs);

  int *N = calloc_or_die(K, sizeof(int));        // counts
  double *S = calloc_or_die(K, sizeof(double));  // sum of samples
  double *S2 = calloc_or_die(K, sizeof(double)); // sum of squared of samples
  double _X;
  for (int k = 0; k < K; k++) {
    for (int i = 0; i < 2; i++) {
      _X = bairvs[k]->sample_func(prng);
      S[k] += _X;
      S2[k] += _X * _X;
      N[k] += 1;
    }
  }

  // FIXME: Initialize identification strategy
  // bai_state = bai_start(sr, N);
  // Sβs = βs

  // FIXME: t in original code is set twice:
  // t = sum(N)
  // t += 1
  // figure out if this is indeed unnecessary
  int t = K * 2;
  double *hμ = calloc_or_die(K, sizeof(double));
  double *hσ2 = calloc_or_die(K, sizeof(double));
  GLRTVars *glrt_vars = GLRTVars_create(K);
  while (true) {
    for (int i = 0; i < K; i++) {
      hμ[i] = S[i] / N[i];
      hσ2[i] = S2[i] / N[i] - hμ[i] * hμ[i];
    }
    glrt(K, N, hμ, hσ2, glrt_vars);
    double *Zs = glrt_vars->vals;
    int aalt = glrt_vars->k;
    int astar = glrt_vars->astar;
    double *ξ = glrt_vars->μ;
    double *ϕ2 = glrt_vars->σ2;
    if (stopping_criterion(K, Zs, Sβ, N, hμ, hσ2, astar)) {
      return astar;
    }
    const int k = nextsample();
    _X = bairvs[k]->sample_func(prng);
    S[k] += _X;
    S2[k] += _X * _X;
    N[k] += 1;
    t += 1;
  }
}