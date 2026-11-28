#include "gp.h"

#include "../def/lamc_defs.h"
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
  // gp_fit_by_likelihood chooses the signal variance among this many values.
  GP_NUM_SIGNAL_VARIANCES = 9,
};

// The signal variances gp_fit_by_likelihood chooses among: standard
// deviations from 0.3 to 32 percentage points about the prior mean, for
// values on the scale of win percentages. They are fixed values, not
// multiples of an estimate from the data: the residuals' variance less the
// noise comes out negative when the samples are few and noisy, such as a
// few rollouts a play that each play the game out and score 0 or 1, and a
// fit on multiples of its floor left every prediction on the prior mean.
// The smallest values still let a fit fall back on the prior mean when the
// samples show nothing beyond it.
static const double gp_signal_variances[GP_NUM_SIGNAL_VARIANCES] = {
    1e-5, 3e-5, 1e-4, 3e-4, 1e-3, 3e-3, 1e-2, 3e-2, 1e-1};

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
  // The length scales gp_fit_by_likelihood tries, num_dims values.
  double *trial_inverse_length_scales;
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
  gp->trial_inverse_length_scales = malloc_or_die(sizeof(double) * num_dims);
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
  free(gp->trial_inverse_length_scales);
  free(gp->points);
  free(gp->lower);
  free(gp->alpha);
  free(gp->scratch);
  free(gp);
}

// The scaled squared distance between first_point and second_point.
static double gp_scaled_distance_squared(const GaussianProcess *gp,
                                         const double *first_point,
                                         const double *second_point) {
  double sum = 0.0;
  for (int dim_idx = 0; dim_idx < gp->num_dims; dim_idx++) {
    const double diff = (first_point[dim_idx] - second_point[dim_idx]) *
                        gp->inverse_length_scales[dim_idx];
    sum += diff * diff;
  }
  return sum;
}

static double gp_kernel(const GaussianProcess *gp, const double *first_point,
                        const double *second_point) {
  const double distance_squared =
      gp_scaled_distance_squared(gp, first_point, second_point);
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

// d k(first_point, second_point) / d first_point[dim], divided by
// (first_point[dim] - second_point[dim]) * inverse_length^2: the same for
// every dimension, so computed once per pair.
static double gp_kernel_gradient_factor(const GaussianProcess *gp,
                                        const double *first_point,
                                        const double *second_point) {
  const double distance_squared =
      gp_scaled_distance_squared(gp, first_point, second_point);
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
  const int num_points = gp->num_points;
  for (int row = 0; row < num_points; row++) {
    const double *row_point = gp->points + (ptrdiff_t)row * gp->num_dims;
    for (int col = 0; col <= row; col++) {
      const double value =
          gp_kernel(gp, row_point, gp->points + (ptrdiff_t)col * gp->num_dims);
      gp->lower[(ptrdiff_t)row * num_points + col] = value;
      gp->lower[(ptrdiff_t)col * num_points + row] = value;
    }
    gp->lower[(ptrdiff_t)row * num_points + row] +=
        noise_variances[row] + jitter;
  }
  return dense_cholesky(gp->lower, num_points);
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

// Sets the trial length scales to the base ones times multiple.
static void gp_set_trial_length_scales(GaussianProcess *gp,
                                       const double *base_inverse_length_scales,
                                       const double multiple) {
  for (int dim_idx = 0; dim_idx < gp->num_dims; dim_idx++) {
    gp->trial_inverse_length_scales[dim_idx] =
        base_inverse_length_scales[dim_idx] / multiple;
  }
}

bool gp_fit_by_likelihood(GaussianProcess *gp, const gp_kernel_t kernel,
                          const double prior_mean,
                          const double *base_inverse_length_scales,
                          const double *length_multiples,
                          const int num_length_multiples, const double *points,
                          const double *values, const double *noise_variances,
                          const int num_points) {
  GaussianProcessParams params = {
      .kernel = kernel,
      .inverse_length_scales = gp->trial_inverse_length_scales,
      .signal_variance = gp_signal_variances[0],
      .prior_mean = prior_mean,
  };
  int best_signal = -1;
  int best_length = -1;
  double best_likelihood = -INFINITY;
  for (int signal_idx = 0; signal_idx < GP_NUM_SIGNAL_VARIANCES; signal_idx++) {
    params.signal_variance = gp_signal_variances[signal_idx];
    for (int length_idx = 0; length_idx < num_length_multiples; length_idx++) {
      gp_set_trial_length_scales(gp, base_inverse_length_scales,
                                 length_multiples[length_idx]);
      if (gp_fit(gp, &params, points, values, noise_variances, num_points) &&
          gp->log_marginal_likelihood > best_likelihood) {
        best_likelihood = gp->log_marginal_likelihood;
        best_signal = signal_idx;
        best_length = length_idx;
      }
    }
  }
  if (best_signal < 0) {
    return false;
  }
  params.signal_variance = gp_signal_variances[best_signal];
  gp_set_trial_length_scales(gp, base_inverse_length_scales,
                             length_multiples[best_length]);
  return gp_fit(gp, &params, points, values, noise_variances, num_points);
}

double gp_get_log_marginal_likelihood(const GaussianProcess *gp) {
  return gp->log_marginal_likelihood;
}

void gp_predict(GaussianProcess *gp, const double *point, double *mean,
                double *variance) {
  const int num_points = gp->num_points;
  double *cross = gp->scratch;
  double mean_value = gp->prior_mean;
  for (int point_idx = 0; point_idx < num_points; point_idx++) {
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
  dense_forward_solve(gp->lower, num_points, cross, cross);
  double explained = 0.0;
  for (int point_idx = 0; point_idx < num_points; point_idx++) {
    explained += cross[point_idx] * cross[point_idx];
  }
  const double posterior = gp->signal_variance - explained;
  *variance = posterior > 0.0 ? posterior : 0.0;
}

void gp_get_bounds(const GaussianProcess *gp, double *mean_bound,
                   double *variance_bound) {
  double positive_weights = 0.0;
  for (int point_idx = 0; point_idx < gp->num_points; point_idx++) {
    if (gp->alpha[point_idx] > 0.0) {
      positive_weights += gp->alpha[point_idx];
    }
  }
  *mean_bound = gp->prior_mean + gp->signal_variance * positive_weights;
  *variance_bound = gp->signal_variance;
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
