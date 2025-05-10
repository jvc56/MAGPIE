#include "math_util.h"

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "../def/math_util_defs.h"

#include "log.h"

double z_to_p_cdf(double z) { return (0.5 * (1 + erf(z / sqrt(2.0)))) * 100; }

// Assumes the correct_sampled_win_pct is between 0.5 and 1 inclusive
// Assumes that a continuity correction from binomial to normal distribution
// has already been applied
double odds_that_player_is_better(double corrected_sampled_win_pct,
                                  int total_games) {
  return z_to_p_cdf((corrected_sampled_win_pct - 0.5) * 2 *
                    sqrt((double)total_games));
}

static double pg_horner(double x, double m, const double *p, int len) {
  double ex = (m + (2 * len - 1)) * (m + (2 * len - 2)) *
              (p[len - 1] / ((2 * len - 1) * (2 * len - 2)));
  for (int i = len - 2; i >= 1; --i) {
    const double pk = p[i];
    const int k = i + 1;
    double cdiv = 1.0 / (double)((2 * k - 1) * (2 * k - 2));
    ex = (cdiv * (m + (2 * k - 1)) * (m + (2 * k - 2))) * (pk + x * ex);
  }
  return (m + 1) * (p[0] + x * ex);
}

// Implemented using gamma.jl from the Julia SpecialFunctions package
double zeta(double s) {
  if (s <= 0.5) {
    log_fatal("zeta function not implemented for values less than or equal to "
              "0.5: %f\n",
              s);
  }

  if (fabs(s - 1.0) < 1e-10) {
    return NAN;
  }
  const double m = s - 1;
  const int n = 6;
  double zeta = 1.0;
  for (int nu = 2; nu <= n; ++nu) {
    double term = pow(1 / (double)nu, s);
    zeta += term;
    if (term < 1e-10) {
      break;
    }
  }

  double z = 1.0 + n;
  double t = 1.0 / z;
  double w = pow(t, m);
  zeta += w * (1.0 / m + 0.5 * t);

  t *= t;
  double coeffs[] = {
      0.08333333333333333,   -0.008333333333333333, 0.003968253968253968,
      -0.004166666666666667, 0.007575757575757576,  -0.021092796092796094,
      0.08333333333333333,   -0.4432598039215686,   3.0539543302701198};
  zeta += w * t * pg_horner(t, m, coeffs, sizeof(coeffs) / sizeof(coeffs[0]));

  return zeta;
}

// Initial approximations for branch 0
double lambertw_branch0(const double x) {
  if (x <= 1) {
    double sqeta = sqrt(2.0 + 2.0 * E * x);
    double N2 =
        3.0 * (SQRT2) + 6.0 -
        (((2237.0 + 1457.0 * (SQRT2)) * E - 4108.0 * (SQRT2)-5764.0) * sqeta) /
            ((215.0 + 199.0 * (SQRT2)) * E - 430.0 * (SQRT2)-796.0);
    double N1 = (1.0 - 1.0 / (SQRT2)) * (N2 + (SQRT2));
    return -1.0 + sqeta / (1.0 + N1 * sqeta / (N2 + sqeta));
  } else {
    return log(6.0 * x /
               (5.0 * log(12.0 / 5.0 * (x / log(1.0 + 12.0 * x / 5.0)))));
  }
}

// Initial approximations for branch -1
double lambertw_branch_neg1(const double x) {
  const double M1 = 0.3361;
  const double M2 = -0.0042;
  const double M3 = -0.0201;
  double sigma = -1.0 - log(-x);
  return -1.0 - sigma -
         2.0 / M1 *
             (1.0 -
              1.0 / (1.0 + (M1 * sqrt(sigma / 2.0)) /
                               (1.0 + M2 * sigma * exp(M3 * sqrt(sigma)))));
}

double lambertw(const double x, const int k) {
  const double minx = -1.0 / E;
  if (x < minx || (k == -1 && x >= 0)) {
    return NAN;
  }

  double W = (k == 0) ? lambertw_branch0(x) : lambertw_branch_neg1(x);
  double r = fabs(W - log(fabs(x)) + log(fabs(W)));
  int n = 1;

  while (r > 1e-10 && n <= 5) {
    double z = log(x / W) - W;
    double q = 2.0 * (1.0 + W) * (1.0 + W + 2.0 / 3.0 * z);
    double epsilon = z * (q - z) / ((1.0 + W) * (q - 2.0 * z));
    W *= 1.0 + epsilon;
    r = fabs(W - log(fabs(x)) + log(fabs(W)));
    n++;
  }

  return W;
}

bool cubic_roots(double a, double b, double c, double d,
                 complex double *roots) {
  if (a == 0) {
    return false;
  }
  const double p = b / a;
  const double q = c / a;
  const double r = d / a;

  const double Q = (3 * q - p * p) / 9.0;
  const double R = (9 * p * q - 27 * r - 2 * p * p * p) / 54.0;

  const double discriminant = Q * Q * Q + R * R;

  if (discriminant >= 0) {
    const double S_real = cbrt(R + sqrt(discriminant));
    const double T_real = cbrt(R - sqrt(discriminant));
    const complex double S = S_real;
    const complex double T = T_real;
    roots[0] = -(S + T) / 2.0 - p / 3.0 - ((S - T) * sqrt(3.0) / 2.0) * I;
    roots[1] = -(S + T) / 2.0 - p / 3.0 + ((S - T) * sqrt(3.0) / 2.0) * I;
    roots[2] = S + T - p / 3.0;
  } else {
    const double theta = acos(R / sqrt(-Q * Q * Q));
    const double sqrt_Q = sqrt(-Q);
    roots[0] = 2 * sqrt_Q * cos((theta + 4 * M_PI) / 3.0) - p / 3.0;
    roots[1] = 2 * sqrt_Q * cos((theta + 2 * M_PI) / 3.0) - p / 3.0;
    roots[2] = 2 * sqrt_Q * cos(theta / 3.0) - p / 3.0;
  }

  return true;
}