#include "pat_gen.h"

#include "../def/pat_defs.h"
#include "../ent/equity.h"
#include "../ent/pat.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static_assert(PAT_FEATURE_HOOK_START == 0,
              "the TWS hook channels start the feature vector");

// Floor on the per-feature variance the ridge penalty scales by (see
// pat_regression_solve_into_weights): keeps the diagonal strictly
// positive for a feature with exactly zero empirical variance (e.g.
// QWS/QLS on a board with no quad squares, whose xtx diagonal is itself
// exactly zero), without meaningfully inflating the penalty for any
// feature with real, nonzero variance -- every premium class's channels
// occur often enough in training data that their true variance sits many
// orders of magnitude above this.
static const double PAT_GEN_RIDGE_VARIANCE_FLOOR = 1e-6;

void pat_regression_reset(PATRegression *regression) {
  memset(regression, 0, sizeof(PATRegression));
}

void pat_regression_add_observation_double(PATRegression *regression,
                                           const double *features,
                                           double label) {
  double row[PAT_REGRESSION_DIM];
  row[0] = 1.0;
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    row[feature_index + 1] = features[feature_index];
  }
  for (int i = 0; i < PAT_REGRESSION_DIM; i++) {
    for (int j = i; j < PAT_REGRESSION_DIM; j++) {
      regression->xtx[i][j] += row[i] * row[j];
    }
    regression->xty[i] += row[i] * label;
  }
  regression->yty += label * label;
  regression->num_observations++;
}

void pat_regression_add_observation(PATRegression *regression,
                                    const int32_t *features,
                                    double reply_score) {
  double row[PAT_REGRESSION_DIM];
  row[0] = 1.0;
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    row[feature_index + 1] = (double)features[feature_index];
  }
  for (int i = 0; i < PAT_REGRESSION_DIM; i++) {
    for (int j = i; j < PAT_REGRESSION_DIM; j++) {
      regression->xtx[i][j] += row[i] * row[j];
    }
    regression->xty[i] += row[i] * reply_score;
  }
  regression->yty += reply_score * reply_score;
  regression->num_observations++;
}

void pat_regression_merge(PATRegression *dst, const PATRegression *src) {
  for (int i = 0; i < PAT_REGRESSION_DIM; i++) {
    for (int j = i; j < PAT_REGRESSION_DIM; j++) {
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
pat_cholesky_decompose(double a[PAT_REGRESSION_DIM][PAT_REGRESSION_DIM]) {
  for (int i = 0; i < PAT_REGRESSION_DIM; i++) {
    for (int j = i; j < PAT_REGRESSION_DIM; j++) {
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
pat_cholesky_solve(const double a[PAT_REGRESSION_DIM][PAT_REGRESSION_DIM],
                   const double *b, double *solution) {
  // Forward substitution: L y = b.
  double y[PAT_REGRESSION_DIM];
  for (int i = 0; i < PAT_REGRESSION_DIM; i++) {
    double sum = b[i];
    for (int k = 0; k < i; k++) {
      sum -= a[i][k] * y[k];
    }
    y[i] = sum / a[i][i];
  }
  // Backward substitution: L^T x = y.
  for (int i = PAT_REGRESSION_DIM - 1; i >= 0; i--) {
    double sum = y[i];
    for (int k = i + 1; k < PAT_REGRESSION_DIM; k++) {
      sum -= a[k][i] * solution[k];
    }
    solution[i] = sum / a[i][i];
  }
}

// E[r^2] and E[r] for r = y - c.x with c the installed coefficients,
// from the accumulated moments (row 0 of XtX holds the feature sums,
// xty[0] the label sum).
static void pat_regression_residual_moments(const PATRegression *regression,
                                            const PATWeights *pat,
                                            double *mean_r2_out,
                                            double *mean_r_out) {
  const double n = (double)regression->num_observations;
  double c[PAT_REGRESSION_DIM];
  c[0] = 0.0;
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    c[feature_index + 1] =
        -equity_to_double(pat_get_weight(pat, feature_index));
  }
  double sum_r2 = regression->yty;
  double sum_r = regression->xty[0];
  for (int i = 1; i < PAT_REGRESSION_DIM; i++) {
    if (c[i] == 0.0) {
      continue;
    }
    sum_r2 -= 2.0 * c[i] * regression->xty[i];
    sum_r -= c[i] * regression->xtx[0][i];
    for (int j = 1; j < PAT_REGRESSION_DIM; j++) {
      if (c[j] == 0.0) {
        continue;
      }
      const double xtx_ij =
          (i <= j) ? regression->xtx[i][j] : regression->xtx[j][i];
      sum_r2 += c[i] * xtx_ij * c[j];
    }
  }
  *mean_r2_out = sum_r2 / n;
  *mean_r_out = sum_r / n;
}

double pat_regression_installed_mse(const PATRegression *regression,
                                    const PATWeights *pat) {
  if (regression->num_observations == 0) {
    return 0.0;
  }
  double mean_r2;
  double mean_r;
  pat_regression_residual_moments(regression, pat, &mean_r2, &mean_r);
  return mean_r2 - mean_r * mean_r;
}

double pat_regression_baseline_mse(const PATRegression *regression) {
  if (regression->num_observations == 0) {
    return 0.0;
  }
  const double n = (double)regression->num_observations;
  const double mean_y = regression->xty[0] / n;
  return regression->yty / n - mean_y * mean_y;
}

PATSolveResult
pat_regression_solve_into_weights(const PATRegression *regression,
                                  double ridge_lambda, PATWeights *pat) {
  PATSolveResult result;
  memset(&result, 0, sizeof(result));
  result.num_observations = regression->num_observations;
  if (regression->num_observations == 0) {
    return result;
  }
  const double num_observations = (double)regression->num_observations;

  // Build the full symmetric ridge system from the accumulated upper
  // triangle. The ridge term scales with the number of observations so
  // lambda has a per-observation meaning; the intercept is unpenalized.
  double a[PAT_REGRESSION_DIM][PAT_REGRESSION_DIM];
  for (int i = 0; i < PAT_REGRESSION_DIM; i++) {
    for (int j = i; j < PAT_REGRESSION_DIM; j++) {
      a[i][j] = regression->xtx[i][j];
      a[j][i] = regression->xtx[i][j];
    }
  }
  // Ridge on standardized features: the penalty for feature i scales with
  // that feature's own empirical variance (row 0 of XtX/xty holds the
  // feature sums, so mean_i = xtx[0][i]/N and variance_i =
  // xtx[i][i]/N - mean_i^2), so classes with a smaller natural scale or a
  // lower occurrence rate than TWS are not shrunk disproportionately. A
  // feature with variance 1 gets the flat per-observation penalty.
  for (int i = 1; i < PAT_REGRESSION_DIM; i++) {
    const double mean_i = regression->xtx[0][i] / num_observations;
    const double variance_i =
        regression->xtx[i][i] / num_observations - mean_i * mean_i;
    const double effective_variance = variance_i > PAT_GEN_RIDGE_VARIANCE_FLOOR
                                          ? variance_i
                                          : PAT_GEN_RIDGE_VARIANCE_FLOOR;
    a[i][i] += ridge_lambda * effective_variance * num_observations;
  }
  // Features held fixed at a value: their contribution moves to the
  // right-hand side (xty_i -= sum_j XtX_ij c_j over fixed j, for every
  // free i), then their column is dropped from the solve by zeroing its
  // row and column with a unit diagonal, so the system stays positive
  // definite; the solution there is overwritten with the fixed value
  // afterwards.
  bool fixed[PAT_REGRESSION_DIM] = {false};
  double fixed_value[PAT_REGRESSION_DIM] = {0.0};
  // The hook-score channels are free only in a PAT_FIT_ALL fit: left free
  // in an ordinary one, the fit moves all hook mass onto them and plays
  // worse than count-weighted hooks. PAT_FIT_THROUGH frees only the
  // floater through channels (for a change to what they measure). A fixed
  // channel keeps its loaded weight, as the coefficient it corresponds to
  // (weights are the negated coefficients, see the clamp below).
  const int fit_residual = pat_get_fit_residual(pat);
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    const bool is_hook_score =
        feature_index >= PAT_FEATURE_HOOK_SCORE_START &&
        feature_index < PAT_FEATURE_HOOK_SCORE_START + PAT_HOOK_BIN_COUNT;
    const bool is_through =
        feature_index >= PAT_FEATURE_FLOAT_THROUGH_SCORE_START &&
        feature_index <
            PAT_FEATURE_FLOAT_THROUGH_COUNT_START + PAT_FLOATER_BIN_COUNT;
    bool free_here = !is_hook_score;
    if (fit_residual == PAT_FIT_ALL) {
      free_here = true;
    } else if (fit_residual == PAT_FIT_THROUGH) {
      free_here = is_through;
    }
    if (!free_here) {
      fixed[feature_index + 1] = true;
      fixed_value[feature_index + 1] =
          -equity_to_double(pat_get_weight(pat, feature_index));
    }
  }
  double xty[PAT_REGRESSION_DIM];
  memcpy(xty, regression->xty, sizeof(xty));
  for (int i = 0; i < PAT_REGRESSION_DIM; i++) {
    if (fixed[i]) {
      continue;
    }
    for (int j = 1; j < PAT_REGRESSION_DIM; j++) {
      if (fixed[j] && fixed_value[j] != 0.0) {
        const double xtx_ij =
            (i <= j) ? regression->xtx[i][j] : regression->xtx[j][i];
        xty[i] -= xtx_ij * fixed_value[j];
      }
    }
  }
  for (int i = 1; i < PAT_REGRESSION_DIM; i++) {
    if (!fixed[i]) {
      continue;
    }
    for (int j = 0; j < PAT_REGRESSION_DIM; j++) {
      a[i][j] = 0.0;
      a[j][i] = 0.0;
    }
    a[i][i] = 1.0;
    xty[i] = 0.0;
  }

  if (!pat_cholesky_decompose(a)) {
    return result;
  }
  double solution[PAT_REGRESSION_DIM];
  pat_cholesky_solve(a, xty, solution);
  for (int i = 1; i < PAT_REGRESSION_DIM; i++) {
    if (fixed[i]) {
      solution[i] = fixed_value[i];
    }
  }

  result.solved = true;
  result.intercept = solution[0];
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    result.coefficients[feature_index] = solution[feature_index + 1];
  }

  // Residual sum of squares: yty - 2 w.Xty + w.XtX.w, computed with the
  // full (unregularized) XtX.
  double fit_rss = regression->yty;
  for (int i = 0; i < PAT_REGRESSION_DIM; i++) {
    fit_rss -= 2.0 * solution[i] * regression->xty[i];
    for (int j = 0; j < PAT_REGRESSION_DIM; j++) {
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
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    double coefficient = result.coefficients[feature_index];
    if (coefficient < 0.0) {
      coefficient = 0.0;
    }
    pat_set_weight(pat, feature_index,
                   equity_negate(double_to_equity(coefficient)));
  }
  pat_bump_mutation_counter(pat);
  return result;
}
