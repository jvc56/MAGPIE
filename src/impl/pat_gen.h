#ifndef PAT_GEN_H
#define PAT_GEN_H

#include "../def/pat_defs.h"
#include "../ent/pat.h"
#include <stdbool.h>
#include <stdint.h>

// Sufficient statistics for the PAT training regression: opponent
// reply score regressed on the post-move defense features plus an
// intercept, accumulated as normal equations. One instance per autoplay
// worker (no locking); merged and solved single-threaded at each
// generation boundary.
enum {
  PAT_REGRESSION_DIM = PAT_NUM_FEATURES + 1,
};

typedef struct PATRegression {
  double xtx[PAT_REGRESSION_DIM][PAT_REGRESSION_DIM];
  double xty[PAT_REGRESSION_DIM];
  double yty;
  uint64_t num_observations;
} PATRegression;

void pat_regression_reset(PATRegression *regression);
// features has PAT_NUM_FEATURES elements; reply_score is in points.
void pat_regression_add_observation(PATRegression *regression,
                                    const int32_t *features,
                                    double reply_score);
// The same, for a feature row that is no longer integral because the units
// were combined with a gamma below one (see pat_extract_features_combined).
void pat_regression_add_observation_double(PATRegression *regression,
                                           const double *features,
                                           double label);
void pat_regression_merge(PATRegression *dst, const PATRegression *src);

typedef struct PATSolveResult {
  bool solved;
  double intercept;
  // Raw (unclamped) regression coefficients per feature, in points of
  // opponent reply score per feature unit.
  double coefficients[PAT_NUM_FEATURES];
  uint64_t num_observations;
  // Mean squared error of the fit and of the intercept-only baseline.
  double mean_squared_error;
  double baseline_mean_squared_error;
} PATSolveResult;

// Solves the ridge normal equations (the intercept is unpenalized) and
// writes the applied weights into pat: coefficients are clamped to >= 0
// (a feature can only be penalized, never rewarded, preserving the <= 0
// sign invariant the shadow pruning relies on) and negated into
// milli-equity. Does not modify pat when the system cannot be solved
// (returns with solved == false), including when there are no
// observations.
PATSolveResult
pat_regression_solve_into_weights(const PATRegression *regression,
                                  double ridge_lambda, PATWeights *pat);
// The same solve with an extra penalty shrink_lambda * N * (c - c0)^2
// pulling every free coefficient toward the weights the file was loaded
// with (c0), so a fit on new data adapts an incumbent instead of
// replacing it; shrink_lambda 0 is the plain solve.
PATSolveResult
pat_regression_solve_into_weights_shrunk(const PATRegression *regression,
                                         double ridge_lambda,
                                         double shrink_lambda, PATWeights *pat);
// Mean squared error of the weights as installed in pat (clamped,
// rounded) on the observations the regression accumulated, with the
// intercept refit; and the intercept-only baseline. Both come straight
// from the accumulated moments, so a held-out accumulator scores any
// candidate without storing rows.
double pat_regression_installed_mse(const PATRegression *regression,
                                    const PATWeights *pat);
// The same with a given intercept (e.g. the one the training fit chose)
// instead of the refit one: untouched out-of-sample error.
double pat_regression_installed_mse_with_intercept(
    const PATRegression *regression, const PATWeights *pat, double intercept);
double pat_regression_baseline_mse(const PATRegression *regression);

#endif
