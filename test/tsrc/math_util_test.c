#include <assert.h>
#include <math.h>

#include "../../src/util/math_util.h"

#include "test_util.h"

void assert_cubic_roots(double a, double b, double c, double d,
                        double *expected_roots) {
  complex double actual_roots[3];
  assert(cubic_roots(a, b, c, d, actual_roots));
  assert(within_epsilon(creal(actual_roots[0]), expected_roots[0]));
  assert(within_epsilon(cimag(actual_roots[0]), expected_roots[1]));
  assert(within_epsilon(creal(actual_roots[1]), expected_roots[2]));
  assert(within_epsilon(cimag(actual_roots[1]), expected_roots[3]));
  assert(within_epsilon(creal(actual_roots[2]), expected_roots[4]));
  assert(within_epsilon(cimag(actual_roots[2]), expected_roots[5]));
}

void test_alt_lambda_cubic_roots(double mu1, double sigma21, double w1,
                                 double mua, double sigma2a, double wa) {
  const double x = wa / w1;
  const double alpha2 = mua + mu1 + (mua + x * mu1) / (1 + x);
  const double alpha1 = (sigma2a + x * sigma21) / (1 + x) + mua * mu1 +
                        (mua + mu1) * (mua + x * mu1) / (1 + x);
  const double alpha0 =
      (mu1 * (mua * mua + sigma2a) + mua * (mu1 * mu1 + sigma21) * x) / (1 + x);

  printf("inputs: %.20f, %.20f, %.20f, %.20f\n", -alpha0, alpha1, -alpha2, 1.0);
  complex double roots[3];
  const bool cubic_root_success =
      cubic_roots(1, -alpha2, alpha1, -alpha0, roots);
  if (!cubic_root_success) {
    log_fatal("cubic solver failed for inputs: %.15f, %.15f, %.15f, %.15f", 1,
              -alpha2, alpha1, -alpha0);
  }
  for (int i = 0; i < 3; i++) {
    printf("%d: %.20f + %.20fi\n", i, creal(roots[i]), cimag(roots[i]));
  }

  for (int i = 0; i < 3; i++) {
    if (fabs(cimag(roots[i])) < 1e-10) {
      const double r = creal(roots[i]);
      printf("r: %.20f ", r);
      printf("r - mua: %.20f ", r - mua);
      printf("r - mu1: %.20f \n", r - mu1);
      if (r - mua >= -1e-10 && r - mu1 <= 1e-10) {
        printf("valid root %d: %.20f + %.20fi\n", i, creal(roots[i]),
               cimag(roots[i]));
      }
    }
  }
}

void test_math_util(void) {
  assert(within_epsilon(odds_that_player_is_better(0.5, 10), 50.0));
  assert(within_epsilon(odds_that_player_is_better(0.5, 100), 50.0));
  assert(within_epsilon(odds_that_player_is_better(0.5, 1000), 50.0));

  assert(within_epsilon(zeta(0.501), -1.4642851751599177));
  assert(within_epsilon(zeta(0.6), -1.9526614482240008));
  assert(within_epsilon(zeta(0.7), -2.7783884455536954));
  assert(within_epsilon(zeta(0.8), -4.437538415895553));
  assert(within_epsilon(zeta(0.9), -9.430114019402254));
  assert(isnan(zeta(1)));
  assert(isnan(zeta(1.0000000000001)));
  assert(within_epsilon(zeta(2), 1.6449340668482273));
  assert(within_epsilon(zeta(3), 1.2020569031595951));
  assert(within_epsilon(zeta(5), 1.0369277551433709));
  assert(within_epsilon(zeta(20), 1.0000009539620338));

  assert(!cubic_roots(0, 1, 1, 1, NULL));
  assert_cubic_roots(
      1, 0, 0, -1,
      (double[]){-0.5, -0.8660254037844389, -0.5, 0.8660254037844389, 1.0, 0});
  assert_cubic_roots(1, 1, 1, 1, (double[]){0.0, -1.0, 0.0, 1.0, -1.0, 0.0});
}
