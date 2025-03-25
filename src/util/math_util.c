/*
MIT License

Copyright (c) 2017-2019 Lakshay Garg <lakshayg@outlook.in>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "math_util.h"
#include <math.h>
#include <stdbool.h>
// FIXME: remove
#include <stdio.h>

#include "../def/math_util_defs.h"

#include "log.h"

// Returns a floating point number y such that std::erf(y)
// is close to x. The current implementation is quite accurate
// when x is away from +1.0 and -1.0. As x approaches closer
// to those values, the error in the result increases.
long double erfinv(long double x) {
  if (x < -1 || x > 1) {
    return NAN;
  } else if (x == 1.0) {
    return INFINITY;
  } else if (x == -1.0) {
    return -INFINITY;
  }

  const long double LN2 = 6.931471805599453094172321214581e-1L;

  const long double A0 = 1.1975323115670912564578e0L;
  const long double A1 = 4.7072688112383978012285e1L;
  const long double A2 = 6.9706266534389598238465e2L;
  const long double A3 = 4.8548868893843886794648e3L;
  const long double A4 = 1.6235862515167575384252e4L;
  const long double A5 = 2.3782041382114385731252e4L;
  const long double A6 = 1.1819493347062294404278e4L;
  const long double A7 = 8.8709406962545514830200e2L;

  const long double B0 = 1.0000000000000000000e0L;
  const long double B1 = 4.2313330701600911252e1L;
  const long double B2 = 6.8718700749205790830e2L;
  const long double B3 = 5.3941960214247511077e3L;
  const long double B4 = 2.1213794301586595867e4L;
  const long double B5 = 3.9307895800092710610e4L;
  const long double B6 = 2.8729085735721942674e4L;
  const long double B7 = 5.2264952788528545610e3L;

  const long double C0 = 1.42343711074968357734e0L;
  const long double C1 = 4.63033784615654529590e0L;
  const long double C2 = 5.76949722146069140550e0L;
  const long double C3 = 3.64784832476320460504e0L;
  const long double C4 = 1.27045825245236838258e0L;
  const long double C5 = 2.41780725177450611770e-1L;
  const long double C6 = 2.27238449892691845833e-2L;
  const long double C7 = 7.74545014278341407640e-4L;

  const long double D0 = 1.4142135623730950488016887e0L;
  const long double D1 = 2.9036514445419946173133295e0L;
  const long double D2 = 2.3707661626024532365971225e0L;
  const long double D3 = 9.7547832001787427186894837e-1L;
  const long double D4 = 2.0945065210512749128288442e-1L;
  const long double D5 = 2.1494160384252876777097297e-2L;
  const long double D6 = 7.7441459065157709165577218e-4L;
  const long double D7 = 1.4859850019840355905497876e-9L;

  const long double E0 = 6.65790464350110377720e0L;
  const long double E1 = 5.46378491116411436990e0L;
  const long double E2 = 1.78482653991729133580e0L;
  const long double E3 = 2.96560571828504891230e-1L;
  const long double E4 = 2.65321895265761230930e-2L;
  const long double E5 = 1.24266094738807843860e-3L;
  const long double E6 = 2.71155556874348757815e-5L;
  const long double E7 = 2.01033439929228813265e-7L;

  const long double F0 = 1.414213562373095048801689e0L;
  const long double F1 = 8.482908416595164588112026e-1L;
  const long double F2 = 1.936480946950659106176712e-1L;
  const long double F3 = 2.103693768272068968719679e-2L;
  const long double F4 = 1.112800997078859844711555e-3L;
  const long double F5 = 2.611088405080593625138020e-5L;
  const long double F6 = 2.010321207683943062279931e-7L;
  const long double F7 = 2.891024605872965461538222e-15L;

  long double abs_x = fabsl(x);

  if (abs_x <= 0.85L) {
    long double r = 0.180625L - 0.25L * x * x;
    long double num =
        (((((((A7 * r + A6) * r + A5) * r + A4) * r + A3) * r + A2) * r + A1) *
             r +
         A0);
    long double den =
        (((((((B7 * r + B6) * r + B5) * r + B4) * r + B3) * r + B2) * r + B1) *
             r +
         B0);
    return x * num / den;
  }

  long double r = sqrtl(LN2 - logl(1.0L - abs_x));

  long double num, den;
  if (r <= 5.0L) {
    r = r - 1.6L;
    num =
        (((((((C7 * r + C6) * r + C5) * r + C4) * r + C3) * r + C2) * r + C1) *
             r +
         C0);
    den =
        (((((((D7 * r + D6) * r + D5) * r + D4) * r + D3) * r + D2) * r + D1) *
             r +
         D0);
  } else {
    r = r - 5.0L;
    num =
        (((((((E7 * r + E6) * r + E5) * r + E4) * r + E3) * r + E2) * r + E1) *
             r +
         E0);
    den =
        (((((((F7 * r + F6) * r + F5) * r + F4) * r + F3) * r + F2) * r + F1) *
             r +
         F0);
  }

  return copysignl(num / den, x);
}

// Refine the result of erfinv by performing Newton-Raphson
// iteration nr_iter number of times. This method works well
// when the value of x is away from 1.0 and -1.0
long double erfinv_refine(long double x, int nr_iter) {
  const long double k = 0.8862269254527580136490837416706L; // 0.5 * sqrt(pi)
  long double y = erfinv(x);
  while (nr_iter-- > 0) {
    y -= k * (erfl(y) - x) / expl(-y * y);
  }
  return y;
}

// Convert a percentage to a z-score
// This implements p = P(-Z<x<Z)
double p_to_z(double p) { return (SQRT2)*erfinv(p / 100); };

double z_to_p_cdf(double z) { return (0.5 * (1 + erf(z / sqrt(2.0)))) * 100; }

// Assumes the correct_sampled_win_pct is between 0.5 and 1 inclusive
// Assumes that a continuity correction from binomial to normal distribution
// has already been applied
double odds_that_player_is_better(double corrected_sampled_win_pct,
                                  int total_games) {
  return z_to_p_cdf((corrected_sampled_win_pct - 0.5) * 2 *
                    sqrt((double)total_games));
}

bool is_z_valid(double zval) { return zval <= p_to_z(PERCENTILE_MAX); }

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

// Returns the number of real roots and fills the roots array
int cubic_roots(double a, double b, double c, double d, double *roots,
                double epsilon) {
  // If a is zero, this is not a cubic equation
  if (fabs(a) < epsilon) {
    return 0;
  }
  // Normalize the equation to standard form x^3 + px^2 + qx + r = 0
  const double p = b / a;
  const double q = c / a;
  const double r = d / a;
  // Calculate intermediate values
  const double Q = (3 * q - p * p) / 9.0;
  const double R = (9 * p * q - 27 * r - 2 * p * p * p) / 54.0;
  // Calculate discriminant
  const double discriminant = Q * Q * Q + R * R;
  // Number of real roots
  int num_roots = 0;
  if (discriminant >= 0) {
    // At least one real root
    const double S = (R >= 0) ? pow(R + sqrt(discriminant), 1.0 / 3.0)
                              : -pow(-R + sqrt(discriminant), 1.0 / 3.0);
    const double T = (Q == 0) ? 0 : Q / S;
    // Adjust roots
    roots[0] = S + T - p / 3.0;
    if (fabs(discriminant) < epsilon) {
      // Two real roots (repeated)
      const double other_root = -roots[0] / 2.0 - p / 3.0;
      roots[1] = other_root;
      roots[2] = other_root;
    } else {
      // One real root
      num_roots = 1;
    }
  } else {
    // Three distinct real roots
    const double theta = acos(R / sqrt(-Q * Q * Q));
    const double sqrt_Q = sqrt(-Q);
    roots[0] = 2 * sqrt_Q * cos(theta / 3.0) - p / 3.0;
    roots[1] = 2 * sqrt_Q * cos((theta + 2 * M_PI) / 3.0) - p / 3.0;
    roots[2] = 2 * sqrt_Q * cos((theta + 4 * M_PI) / 3.0) - p / 3.0;
    num_roots = 3;
  }
  // Validate the roots
  int num_valid_roots = 0;
  for (int i = 0; i < num_roots; i++) {
    if (fabs(roots[i]) < epsilon) {
      roots[num_valid_roots++] = roots[i];
    }
  }
  return num_valid_roots;
}