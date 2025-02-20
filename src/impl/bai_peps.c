#include "bai_peps.h"

#include <float.h>
#include <stdbool.h>

#define BAI_EPSILON 0.0000001

GLRTVars *bai_glrt_vars_create(int K) {
  GLRTVars *glrt_vars = malloc_or_die(sizeof(GLRTVars));
  glrt_vars->vals = malloc_or_die(K * sizeof(double));
  glrt_vars->θs = malloc_or_die(K * sizeof(double));
  glrt_vars->λ = malloc_or_die(K * sizeof(double));
  glrt_vars->ϕ2 = malloc_or_die(K * sizeof(double));
  glrt_vars->μ = malloc_or_die(K * sizeof(double));
  glrt_vars->σ2 = malloc_or_die(K * sizeof(double));
  return glrt_vars;
}

void bai_glrt_vars_destroy(GLRTVars *glrt_vars) {
  free(glrt_vars->vals);
  free(glrt_vars->θs);
  free(glrt_vars->λ);
  free(glrt_vars->ϕ2);
  free(glrt_vars->μ);
  free(glrt_vars->σ2);
  free(glrt_vars);
}

double alt_λ_KV(double μ1, double σ21, int w1, double μa, double σ2a, int wa) {
  if (w1 == 0) {
    return μa;
  }
  // FIXME: check if this actually does need true equality comparison here
  if (wa == 0 || (abs(μ1 - μa) < BAI_EPSILON)) {
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

void bai_glrt(int K, int *w, double *μ, double *σ2, GLRTVars *glrt_vars) {
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

// Creates a double array which the call is responsible for freeing.
double *bai_oracle(double *μs, double *σ2s, int size) {
  double μstar = μs[0];
  for (int i = 1; i < size; i++) {
    if (μs[i] > μstar) {
      μstar = μs[i];
    }
  }
  // FIXME: implement plz
}