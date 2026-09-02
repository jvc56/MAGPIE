#include "twd_gen.h"

#include "../def/tws_defense_defs.h"
#include "../ent/equity.h"
#include "../ent/tws_defense.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void twd_regression_reset(TWDRegression *regression) {
  memset(regression, 0, sizeof(TWDRegression));
}

void twd_regression_add_observation_double(TWDRegression *regression,
                                           const double *features,
                                           double label) {
  double row[TWD_REGRESSION_DIM];
  row[0] = 1.0;
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    row[feature_index + 1] = features[feature_index];
  }
  for (int i = 0; i < TWD_REGRESSION_DIM; i++) {
    for (int j = i; j < TWD_REGRESSION_DIM; j++) {
      regression->xtx[i][j] += row[i] * row[j];
    }
    regression->xty[i] += row[i] * label;
  }
  regression->yty += label * label;
  regression->num_observations++;
}

void twd_regression_add_observation(TWDRegression *regression,
                                    const int32_t *features,
                                    double reply_score) {
  double row[TWD_REGRESSION_DIM];
  row[0] = 1.0;
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    row[feature_index + 1] = (double)features[feature_index];
  }
  for (int i = 0; i < TWD_REGRESSION_DIM; i++) {
    for (int j = i; j < TWD_REGRESSION_DIM; j++) {
      regression->xtx[i][j] += row[i] * row[j];
    }
    regression->xty[i] += row[i] * reply_score;
  }
  regression->yty += reply_score * reply_score;
  regression->num_observations++;
}

void twd_regression_merge(TWDRegression *dst, const TWDRegression *src) {
  for (int i = 0; i < TWD_REGRESSION_DIM; i++) {
    for (int j = i; j < TWD_REGRESSION_DIM; j++) {
      dst->xtx[i][j] += src->xtx[i][j];
    }
    dst->xty[i] += src->xty[i];
  }
  dst->yty += src->yty;
  dst->num_observations += src->num_observations;
}

// Cholesky decomposition of the symmetric positive definite matrix stored
// in the upper triangle of a; on success the lower triangle holds L with
// a[i][i] the diagonal of L. Returns false if the matrix is not positive
// definite (a singular or degenerate system).
static bool
twd_cholesky_decompose(double a[TWD_REGRESSION_DIM][TWD_REGRESSION_DIM]) {
  for (int i = 0; i < TWD_REGRESSION_DIM; i++) {
    for (int j = i; j < TWD_REGRESSION_DIM; j++) {
      double sum = a[i][j];
      for (int k = 0; k < i; k++) {
        sum -= a[i][k] * a[j][k];
      }
      if (i == j) {
        if (sum <= 0.0) {
          return false;
        }
        a[i][i] = sqrt(sum);
      } else {
        a[j][i] = sum / a[i][i];
      }
    }
  }
  return true;
}

static void
twd_cholesky_solve(const double a[TWD_REGRESSION_DIM][TWD_REGRESSION_DIM],
                   const double *b, double *solution) {
  // Forward substitution: L y = b.
  double y[TWD_REGRESSION_DIM];
  for (int i = 0; i < TWD_REGRESSION_DIM; i++) {
    double sum = b[i];
    for (int k = 0; k < i; k++) {
      sum -= a[i][k] * y[k];
    }
    y[i] = sum / a[i][i];
  }
  // Backward substitution: L^T x = y.
  for (int i = TWD_REGRESSION_DIM - 1; i >= 0; i--) {
    double sum = y[i];
    for (int k = i + 1; k < TWD_REGRESSION_DIM; k++) {
      sum -= a[k][i] * solution[k];
    }
    solution[i] = sum / a[i][i];
  }
}

TWDSolveResult
twd_regression_solve_into_weights(const TWDRegression *regression,
                                  double ridge_lambda, TWDWeights *twd) {
  TWDSolveResult result;
  memset(&result, 0, sizeof(result));
  result.num_observations = regression->num_observations;
  if (regression->num_observations == 0) {
    return result;
  }
  const double num_observations = (double)regression->num_observations;

  // Build the full symmetric ridge system from the accumulated upper
  // triangle. The ridge term scales with the number of observations so
  // lambda has a per-observation meaning; the intercept is unpenalized.
  double a[TWD_REGRESSION_DIM][TWD_REGRESSION_DIM];
  for (int i = 0; i < TWD_REGRESSION_DIM; i++) {
    for (int j = i; j < TWD_REGRESSION_DIM; j++) {
      a[i][j] = regression->xtx[i][j];
      a[j][i] = regression->xtx[i][j];
    }
  }
  for (int i = 1; i < TWD_REGRESSION_DIM; i++) {
    a[i][i] += ridge_lambda * num_observations;
  }

  if (!twd_cholesky_decompose(a)) {
    return result;
  }
  double solution[TWD_REGRESSION_DIM];
  twd_cholesky_solve(a, regression->xty, solution);

  result.solved = true;
  result.intercept = solution[0];
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    result.coefficients[feature_index] = solution[feature_index + 1];
  }

  // Residual sum of squares: yty - 2 w.Xty + w.XtX.w, computed with the
  // full (unregularized) XtX.
  double fit_rss = regression->yty;
  for (int i = 0; i < TWD_REGRESSION_DIM; i++) {
    fit_rss -= 2.0 * solution[i] * regression->xty[i];
    for (int j = 0; j < TWD_REGRESSION_DIM; j++) {
      const double xtx_ij =
          (i <= j) ? regression->xtx[i][j] : regression->xtx[j][i];
      fit_rss += solution[i] * xtx_ij * solution[j];
    }
  }
  result.mean_squared_error = fit_rss / num_observations;
  const double mean_reply = regression->xty[0] / num_observations;
  result.baseline_mean_squared_error =
      regression->yty / num_observations - mean_reply * mean_reply;

  // A positive coefficient means the feature is associated with a higher
  // opponent reply score, so it becomes a penalty of that many points per
  // feature unit. Negative coefficients clamp to zero: applied weights must
  // be <= 0 (see the shadow invariant in static_eval.h).
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    double coefficient = result.coefficients[feature_index];
    if (coefficient < 0.0) {
      coefficient = 0.0;
    }
    twd_set_weight(twd, feature_index,
                   equity_negate(double_to_equity(coefficient)));
  }
  twd_bump_mutation_counter(twd);
  return result;
}
