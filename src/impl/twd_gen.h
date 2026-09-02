#ifndef TWD_GEN_H
#define TWD_GEN_H

#include "../def/tws_defense_defs.h"
#include "../ent/tws_defense.h"
#include <stdbool.h>
#include <stdint.h>

// Sufficient statistics for the TWS defense training regression: opponent
// reply score regressed on the post-move defense features plus an
// intercept, accumulated as normal equations. One instance per autoplay
// worker (no locking); merged and solved single-threaded at each
// generation boundary.
enum {
  TWD_REGRESSION_DIM = TWD_NUM_FEATURES + 1,
};

typedef struct TWDRegression {
  double xtx[TWD_REGRESSION_DIM][TWD_REGRESSION_DIM];
  double xty[TWD_REGRESSION_DIM];
  double yty;
  uint64_t num_observations;
} TWDRegression;

void twd_regression_reset(TWDRegression *regression);
// features has TWD_NUM_FEATURES elements; reply_score is in points.
void twd_regression_add_observation(TWDRegression *regression,
                                    const int32_t *features,
                                    double reply_score);
// The same, for a feature row that is no longer integral because the units
// were combined with a gamma below one (see twd_extract_features_combined).
void twd_regression_add_observation_double(TWDRegression *regression,
                                           const double *features,
                                           double label);
void twd_regression_merge(TWDRegression *dst, const TWDRegression *src);

typedef struct TWDSolveResult {
  bool solved;
  double intercept;
  // Raw (unclamped) regression coefficients per feature, in points of
  // opponent reply score per feature unit.
  double coefficients[TWD_NUM_FEATURES];
  uint64_t num_observations;
  // Mean squared error of the fit and of the intercept-only baseline.
  double mean_squared_error;
  double baseline_mean_squared_error;
} TWDSolveResult;

// Solves the ridge normal equations (the intercept is unpenalized) and
// writes the applied weights into twd: coefficients are clamped to >= 0
// (a feature can only be penalized, never rewarded, preserving the <= 0
// sign invariant the shadow pruning relies on) and negated into
// milli-equity. Does not modify twd when the system cannot be solved
// (returns with solved == false), including when there are no
// observations.
TWDSolveResult
twd_regression_solve_into_weights(const TWDRegression *regression,
                                  double ridge_lambda, TWDWeights *twd);

#endif
