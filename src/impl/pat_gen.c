#include "pat_gen.h"

#include "../def/pat_defs.h"
#include "../ent/equity.h"
#include "../ent/pat.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

double pat_regression_installed_mse(const PATRegression *regression,
                                    const PATWeights *pat) {
  if (regression->num_observations == 0) {
    return 0.0;
  }
  const double n = (double)regression->num_observations;
  // The installed coefficients: weights are the negated coefficients in
  // milli-equity (see the clamp at the end of the solve).
  double c[PAT_REGRESSION_DIM];
  c[0] = 0.0;
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    c[feature_index + 1] =
        -equity_to_double(pat_get_weight(pat, feature_index));
  }
  // E[r^2] and E[r] for r = y - c.x, from the accumulated moments (row 0
  // of XtX holds the feature sums, xty[0] the label sum); the intercept
  // is refit implicitly by subtracting E[r]^2, since it never changes a
  // move choice.
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
  const double mean_r = sum_r / n;
  return sum_r2 / n - mean_r * mean_r;
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
  for (int i = 1; i < PAT_REGRESSION_DIM; i++) {
    a[i][i] += (ridge_lambda + shrink_lambda) * num_observations;
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
  //     a pair to count-weighted hooks in whole-game play. CSW21's v3
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
  if (residual_mode != 1 && residual_mode != 2) {
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
  if (pat_get_fit_residual(pat)) {
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
  // shrink_lambda * N * (c - loaded)^2 adds shrink_lambda * N to the
  // diagonal (above) and shrink_lambda * N * loaded to the right-hand
  // side.
  if (shrink_lambda > 0.0) {
    for (int i = 1; i < PAT_REGRESSION_DIM; i++) {
      xty[i] += shrink_lambda * num_observations * loaded[i];
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
