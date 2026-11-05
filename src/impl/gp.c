#include "gp.h"

#include "../util/dense_linalg.h"
#include "../util/io_util.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Jitter is tried at most this many times, growing tenfold each time.
  GP_MAX_JITTER_ATTEMPTS = 6,
};

#define GP_INITIAL_JITTER 1e-10
#define GP_SQRT5 2.23606797749978969641
#define GP_LOG_TWO_PI 1.83787706640934548356

struct GaussianProcess {
  int max_points;
  int num_dims;
  int num_points;
  gp_kernel_t kernel;
  double signal_variance;
  double prior_mean;
  double log_marginal_likelihood;
  double *inverse_length_scales;
  double *points;
  // L, the Cholesky factor of K + noise, num_points x num_points.
  double *lower;
  // (K + noise)^-1 (values - prior_mean).
  double *alpha;
  double *scratch;
};

GaussianProcess *gp_create(const int max_points, const int num_dims) {
  GaussianProcess *gp = malloc_or_die(sizeof(GaussianProcess));
  gp->max_points = max_points;
  gp->num_dims = num_dims;
  gp->num_points = 0;
  gp->kernel = GP_KERNEL_MATERN52;
  gp->signal_variance = 1.0;
  gp->prior_mean = 0.0;
  gp->log_marginal_likelihood = 0.0;
  gp->inverse_length_scales = malloc_or_die(sizeof(double) * num_dims);
  gp->points = malloc_or_die(sizeof(double) * (size_t)max_points * num_dims);
  gp->lower = malloc_or_die(sizeof(double) * (size_t)max_points * max_points);
  gp->alpha = malloc_or_die(sizeof(double) * max_points);
  gp->scratch = malloc_or_die(sizeof(double) * max_points);
  return gp;
}

void gp_destroy(GaussianProcess *gp) {
  if (!gp) {
    return;
  }
  free(gp->inverse_length_scales);
  free(gp->points);
  free(gp->lower);
  free(gp->alpha);
  free(gp->scratch);
  free(gp);
}

// The scaled squared distance between a and b.
static double gp_scaled_distance_squared(const GaussianProcess *gp,
                                         const double *a, const double *b) {
  double sum = 0.0;
  for (int dim_idx = 0; dim_idx < gp->num_dims; dim_idx++) {
    const double diff =
        (a[dim_idx] - b[dim_idx]) * gp->inverse_length_scales[dim_idx];
    sum += diff * diff;
  }
  return sum;
}

static double gp_kernel(const GaussianProcess *gp, const double *a,
                        const double *b) {
  const double distance_squared = gp_scaled_distance_squared(gp, a, b);
  switch (gp->kernel) {
  case GP_KERNEL_MATERN52: {
    const double scaled = GP_SQRT5 * sqrt(distance_squared);
    return gp->signal_variance *
           (1.0 + scaled + (5.0 / 3.0) * distance_squared) * exp(-scaled);
  }
  case GP_KERNEL_SQUARED_EXPONENTIAL:
    return gp->signal_variance * exp(-0.5 * distance_squared);
  }
  log_fatal("unknown gaussian process kernel %d", (int)gp->kernel);
  return 0.0;
}

// d k(a, b) / d a[dim], divided by (a[dim] - b[dim]) * inverse_length^2:
// the same for every dimension, so computed once per pair.
static double gp_kernel_gradient_factor(const GaussianProcess *gp,
                                        const double *a, const double *b) {
  const double distance_squared = gp_scaled_distance_squared(gp, a, b);
  switch (gp->kernel) {
  case GP_KERNEL_MATERN52: {
    const double scaled = GP_SQRT5 * sqrt(distance_squared);
    return -gp->signal_variance * (5.0 / 3.0) * (1.0 + scaled) * exp(-scaled);
  }
  case GP_KERNEL_SQUARED_EXPONENTIAL:
    return -gp->signal_variance * exp(-0.5 * distance_squared);
  }
  log_fatal("unknown gaussian process kernel %d", (int)gp->kernel);
  return 0.0;
}

static bool gp_factor(GaussianProcess *gp, const double *noise_variances,
                      const double jitter) {
  const int n = gp->num_points;
  for (int row = 0; row < n; row++) {
    const double *row_point = gp->points + (ptrdiff_t)row * gp->num_dims;
    for (int col = 0; col <= row; col++) {
      const double value =
          gp_kernel(gp, row_point, gp->points + (ptrdiff_t)col * gp->num_dims);
      gp->lower[(ptrdiff_t)row * n + col] = value;
      gp->lower[(ptrdiff_t)col * n + row] = value;
    }
    gp->lower[(ptrdiff_t)row * n + row] += noise_variances[row] + jitter;
  }
  return dense_cholesky(gp->lower, n);
}

bool gp_fit(GaussianProcess *gp, const GaussianProcessParams *params,
            const double *points, const double *values,
            const double *noise_variances, const int num_points) {
  if (num_points > gp->max_points) {
    log_fatal("gaussian process fit of %d points exceeds its capacity of %d",
              num_points, gp->max_points);
  }
  gp->num_points = num_points;
  gp->kernel = params->kernel;
  gp->signal_variance = params->signal_variance;
  gp->prior_mean = params->prior_mean;
  memcpy(gp->inverse_length_scales, params->inverse_length_scales,
         sizeof(double) * gp->num_dims);
  memcpy(gp->points, points,
         sizeof(double) * (size_t)num_points * gp->num_dims);

  bool factored = false;
  double jitter = 0.0;
  for (int attempt = 0; attempt <= GP_MAX_JITTER_ATTEMPTS && !factored;
       attempt++) {
    factored = gp_factor(gp, noise_variances, jitter);
    jitter =
        jitter == 0.0 ? GP_INITIAL_JITTER * gp->signal_variance : jitter * 10.0;
  }
  if (!factored) {
    gp->num_points = 0;
    return false;
  }

  for (int point_idx = 0; point_idx < num_points; point_idx++) {
    gp->scratch[point_idx] = values[point_idx] - gp->prior_mean;
  }
  dense_forward_solve(gp->lower, num_points, gp->scratch, gp->alpha);
  double fit_term = 0.0;
  double log_determinant = 0.0;
  for (int point_idx = 0; point_idx < num_points; point_idx++) {
    fit_term += gp->alpha[point_idx] * gp->alpha[point_idx];
    log_determinant +=
        log(gp->lower[(ptrdiff_t)point_idx * num_points + point_idx]);
  }
  dense_backward_solve(gp->lower, num_points, gp->alpha, gp->alpha);
  gp->log_marginal_likelihood =
      -0.5 * fit_term - log_determinant - 0.5 * num_points * GP_LOG_TWO_PI;
  return true;
}

double gp_get_log_marginal_likelihood(const GaussianProcess *gp) {
  return gp->log_marginal_likelihood;
}

void gp_predict(GaussianProcess *gp, const double *point, double *mean,
                double *variance) {
  const int n = gp->num_points;
  double *cross = gp->scratch;
  double mean_value = gp->prior_mean;
  for (int point_idx = 0; point_idx < n; point_idx++) {
    cross[point_idx] =
        gp_kernel(gp, point, gp->points + (ptrdiff_t)point_idx * gp->num_dims);
    mean_value += cross[point_idx] * gp->alpha[point_idx];
  }
  if (mean) {
    *mean = mean_value;
  }
  if (!variance) {
    return;
  }
  dense_forward_solve(gp->lower, n, cross, cross);
  double explained = 0.0;
  for (int point_idx = 0; point_idx < n; point_idx++) {
    explained += cross[point_idx] * cross[point_idx];
  }
  const double posterior = gp->signal_variance - explained;
  *variance = posterior > 0.0 ? posterior : 0.0;
}

void gp_mean_gradient(const GaussianProcess *gp, const double *point,
                      double *gradient) {
  for (int dim_idx = 0; dim_idx < gp->num_dims; dim_idx++) {
    gradient[dim_idx] = 0.0;
  }
  for (int point_idx = 0; point_idx < gp->num_points; point_idx++) {
    const double *other = gp->points + (ptrdiff_t)point_idx * gp->num_dims;
    const double factor =
        gp_kernel_gradient_factor(gp, point, other) * gp->alpha[point_idx];
    for (int dim_idx = 0; dim_idx < gp->num_dims; dim_idx++) {
      const double inverse_length = gp->inverse_length_scales[dim_idx];
      gradient[dim_idx] += factor * (point[dim_idx] - other[dim_idx]) *
                           inverse_length * inverse_length;
    }
  }
}
