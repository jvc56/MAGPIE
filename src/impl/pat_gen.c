#include "pat_gen.h"

#include "../def/pat_defs.h"
#include "../ent/equity.h"
#include "../ent/pat.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Floor on the per-feature variance the ridge penalty scales by (see
// pat_regression_solve_into_weights_shrunk): keeps the diagonal strictly
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

PATSolveResult
pat_regression_solve_into_weights(const PATRegression *regression,
                                  double ridge_lambda, PATWeights *pat) {
  return pat_regression_solve_into_weights_shrunk(regression, ridge_lambda, 0.0,
                                                  pat);
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

double pat_regression_installed_mse_with_intercept(
    const PATRegression *regression, const PATWeights *pat, double intercept) {
  if (regression->num_observations == 0) {
    return 0.0;
  }
  double mean_r2;
  double mean_r;
  pat_regression_residual_moments(regression, pat, &mean_r2, &mean_r);
  // E[(r - b)^2] = E[r^2] - 2 b E[r] + b^2.
  return mean_r2 - 2.0 * intercept * mean_r + intercept * intercept;
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

PATSolveResult pat_regression_solve_into_weights_shrunk(
    const PATRegression *regression, double ridge_lambda, double shrink_lambda,
    PATWeights *pat) {
  PATSolveResult result;
  memset(&result, 0, sizeof(result));
  result.num_observations = regression->num_observations;
  if (regression->num_observations == 0) {
    return result;
  }
  const double num_observations = (double)regression->num_observations;
  // The loaded coefficients, the target the shrinkage pulls toward (the
  // plain ridge pulls toward zero, shrink_lambda 0 leaves it at that).
  double loaded[PAT_REGRESSION_DIM];
  loaded[0] = 0.0;
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    loaded[feature_index + 1] =
        -equity_to_double(pat_get_weight(pat, feature_index));
  }

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
  // xtx[i][i]/N - mean_i^2) rather than a flat per-observation constant.
  // A flat penalty implicitly assumes every feature has unit variance;
  // premium classes with a smaller natural scale or a lower occurrence
  // rate than TWS (which is what every non-TWS class is, by construction
  // -- see pat_defs.h) would otherwise absorb a disproportionately larger
  // *relative* shrinkage than TWS's own channels for the exact same
  // ridge_lambda, which is the standard ridge-regression pathology of
  // penalizing unstandardized features with a single lambda. Scaling by
  // each feature's own variance removes that asymmetry: a reference
  // feature with variance 1 gets exactly the old behavior, so
  // ridge_lambda's calibrated scale carries over unchanged.
  for (int i = 1; i < PAT_REGRESSION_DIM; i++) {
    const double mean_i = regression->xtx[0][i] / num_observations;
    const double variance_i =
        regression->xtx[i][i] / num_observations - mean_i * mean_i;
    const double effective_variance = variance_i > PAT_GEN_RIDGE_VARIANCE_FLOOR
                                          ? variance_i
                                          : PAT_GEN_RIDGE_VARIANCE_FLOOR;
    a[i][i] +=
        (ridge_lambda + shrink_lambda) * effective_variance * num_observations;
  }
  // Features held fixed at a value: their contribution moves to the
  // right-hand side (xty_i -= sum_j XtX_ij c_j over fixed j, for every
  // free i), then their column is dropped from the solve by zeroing its
  // row and column with a unit diagonal, so the system stays positive
  // definite; the solution there is overwritten with the fixed value
  // afterwards. Excluded features are the fixed-at-zero case.
  bool fixed[PAT_REGRESSION_DIM] = {false};
  double fixed_value[PAT_REGRESSION_DIM] = {0.0};
  // Experimental channels keep whatever the file carries (zero for every
  // file so far) unless the residual mode that studies them is on, so a
  // default fit never spends mass on them:
  //   - the premium-combination channels (fit_residual 4): a residual on
  //     v4 was null;
  //   - the hook-score channels (fit_residual 1 or 2): left free, a fit
  //     moves every bit of hook mass onto them (hook_d* all zero,
  //     hook_score_d1 about -130) and that model loses about 2.6 points
  //     a pair to count-weighted hooks in whole-game play. Mode 5 explicitly
  //     frees them in a full experimental fit. CSW21's v3
  //     predates these channels; a lexicon trained after them was first
  //     built that way by mistake (2026-09-14) and came out at a third
  //     of the strength.
  const int residual_mode = pat_get_fit_residual_mode(pat);
  if (residual_mode != 4) {
    for (int feature_index = PAT_FEATURE_LM_SPAN_START;
         feature_index < PAT_NUM_FEATURES; feature_index++) {
      fixed[feature_index + 1] = true;
      fixed_value[feature_index + 1] =
          -equity_to_double(pat_get_weight(pat, feature_index));
    }
  }
  if (residual_mode != 1 && residual_mode != 2 && residual_mode != 5) {
    for (int feature_index = PAT_FEATURE_HOOK_SCORE_START;
         feature_index < PAT_FEATURE_HOOK_SCORE_START + PAT_HOOK_BIN_COUNT;
         feature_index++) {
      fixed[feature_index + 1] = true;
      fixed_value[feature_index + 1] =
          -equity_to_double(pat_get_weight(pat, feature_index));
    }
  }
  if (!pat_get_fit_scaled_channels(pat)) {
    for (int feature_index = PAT_FEATURE_HOOK_SCALED_START;
         feature_index < PAT_FEATURE_HOOK_SCORE_START; feature_index++) {
      fixed[feature_index + 1] = true;
    }
  }
  if (pat_get_fit_residual(pat) && residual_mode != 5) {
    // Only the hook-score channels move -- and, with fit_residual 2, the
    // triple-word hook flexibility channels they are collinear with, so
    // the fit can shift mass between count-weighted and score-weighted
    // hooks; everything else keeps the loaded weight, as the coefficient
    // it corresponds to (weights are the negated coefficients, see the
    // clamp below).
    // Mode 3 frees the floater through channels instead (for a semantic
    // change to what they measure), keeping every hook channel fixed;
    // mode 4 only the premium-combination channels.
    const int mode = pat_get_fit_residual_mode(pat);
    const bool hooks_free = mode == 2;
    const bool through_free = mode == 3;
    const bool lm_free = mode == 4;
    for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
         feature_index++) {
      const bool is_hook_score =
          feature_index >= PAT_FEATURE_HOOK_SCORE_START &&
          feature_index < PAT_FEATURE_HOOK_SCORE_START + PAT_HOOK_BIN_COUNT;
      const bool is_tws_hook =
          feature_index >= PAT_FEATURE_HOOK_START &&
          feature_index < PAT_FEATURE_HOOK_START + PAT_HOOK_BIN_COUNT;
      const bool is_through =
          feature_index >= PAT_FEATURE_FLOAT_THROUGH_SCORE_START &&
          feature_index <
              PAT_FEATURE_FLOAT_THROUGH_COUNT_START + PAT_FLOATER_BIN_COUNT;
      const bool is_lm = feature_index >= PAT_FEATURE_LM_SPAN_START;
      const bool free_here =
          lm_free        ? is_lm
          : through_free ? is_through
                         : (is_hook_score || (hooks_free && is_tws_hook));
      if (!free_here) {
        fixed[feature_index + 1] = true;
        fixed_value[feature_index + 1] =
            -equity_to_double(pat_get_weight(pat, feature_index));
      }
    }
  }
  double xty[PAT_REGRESSION_DIM];
  memcpy(xty, regression->xty, sizeof(xty));
  // Shrinkage toward the loaded coefficients: the penalty
  // shrink_lambda * variance_i * N * (c - loaded)^2 adds the same
  // variance-scaled strength to the diagonal (above) and to the loaded
  // coefficient on the right-hand side.
  if (shrink_lambda > 0.0) {
    for (int i = 1; i < PAT_REGRESSION_DIM; i++) {
      const double mean_i = regression->xtx[0][i] / num_observations;
      const double variance_i =
          regression->xtx[i][i] / num_observations - mean_i * mean_i;
      const double effective_variance =
          variance_i > PAT_GEN_RIDGE_VARIANCE_FLOOR
              ? variance_i
              : PAT_GEN_RIDGE_VARIANCE_FLOOR;
      xty[i] +=
          shrink_lambda * effective_variance * num_observations * loaded[i];
    }
  }
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
