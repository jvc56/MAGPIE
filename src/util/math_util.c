#include "math_util.h"

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "io_util.h"

#define M_PI 3.14159265358979323846

double z_to_p_cdf(double z) { return (0.5 * (1 + erf(z / sqrt(2.0)))) * 100; }

// Assumes the correct_sampled_win_pct is between 0.5 and 1 inclusive
// Assumes that a continuity correction from binomial to normal distribution
// has already been applied
double odds_that_player_is_better(double corrected_sampled_win_pct,
                                  uint64_t total_games) {
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
              "0.5: %f",
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

uint64_t choose(uint64_t n, uint64_t k) {
  if (n < k) {
    return 0;
  }
  if (k == 0 || n == k) {
    return 1;
  }
  if (k > n - k) {
    k = n - k;
  }
  uint64_t result = 1;
  for (uint64_t i = 1; i <= k; ++i) {
    result = result * (n - i + 1) / i;
  }
  return result;
}
