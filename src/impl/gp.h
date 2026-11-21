#ifndef GP_H
#define GP_H

#include "../def/lamc_defs.h"
#include <stdbool.h>

// Gaussian process regression for Locally Adaptive Monte Carlo (lamc.h):
// fits observed values at points of a feature space, with a known noise
// variance per point, and predicts the mean and variance elsewhere.

typedef struct GaussianProcess GaussianProcess;

typedef struct GaussianProcessParams {
  gp_kernel_t kernel;
  // Per dimension: 1 / length scale. 0 leaves the dimension out.
  const double *inverse_length_scales;
  double signal_variance;
  // The prior mean, the same everywhere.
  double prior_mean;
} GaussianProcessParams;

GaussianProcess *gp_create(int max_points, int num_dims);
void gp_destroy(GaussianProcess *gp);

// Fits num_points observations: points (num_points x num_dims, row-major),
// their values, and each value's noise variance. The points are copied.
// Adds jitter to the diagonal if the kernel matrix is not numerically
// positive definite, and returns false only if that fails too.
bool gp_fit(GaussianProcess *gp, const GaussianProcessParams *params,
            const double *points, const double *values,
            const double *noise_variances, int num_points);

// The log marginal likelihood of the last fit's values.
double gp_get_log_marginal_likelihood(const GaussianProcess *gp);

// The posterior mean and variance at point. Either output may be NULL. Uses
// the process's scratch space, so one process predicts on one thread at a
// time.
void gp_predict(GaussianProcess *gp, const double *point, double *mean,
                double *variance);

// The gradient of the posterior mean at point, num_dims values.
void gp_mean_gradient(const GaussianProcess *gp, const double *point,
                      double *gradient);

#endif
