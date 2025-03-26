#include <assert.h>
#include <math.h>

#include "../../src/def/math_util_defs.h"
#include "../../src/impl/bai_peps.h"

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

void test_math_util(void) {
  // This also tests ervinv
  assert(within_epsilon(p_to_z(95), 1.959964));
  assert(within_epsilon(p_to_z(98), 2.326348));
  assert(within_epsilon(p_to_z(99), 2.575829));

  assert(is_z_valid(p_to_z(PERCENTILE_MAX)));
  assert(!is_z_valid(p_to_z(PERCENTILE_MAX + (100.0 - PERCENTILE_MAX) / 2.0)));

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

  assert(within_epsilon(lambertw(-1 / E, 0), -1.0));
  assert(within_epsilon(lambertw(-0.1, 0), -0.11183255915896297));
  assert(within_epsilon(lambertw(0.0, 0), 0));
  assert(within_epsilon(lambertw(1.0, 0), 0.5671432904097838));
  assert(within_epsilon(lambertw(2.0, 0), 0.8526055020137254));
  assert(within_epsilon(lambertw(3.0, 0), 1.0499088949640398));
  assert(within_epsilon(lambertw(4.0, 0), 1.2021678731970429));
  assert(within_epsilon(lambertw(5.0, 0), 1.3267246652422002));
  assert(within_epsilon(lambertw(6.0, 0), 1.4324047758983003));
  assert(within_epsilon(lambertw(1000.0, 0), 5.249602852401596));
  assert(within_epsilon(lambertw(-1 / E, -1), -1.0));
  assert(within_epsilon(lambertw(-0.1, -1), -3.577152063957297));
  assert(within_epsilon(lambertw(-0.01, -1), -6.472775124394005));
  assert(isnan(lambertw(0.0, -1)));

  assert(!cubic_roots(0, 1, 1, 1, NULL));
  assert_cubic_roots(
      1, 0, 0, -1,
      (double[]){-0.5, -0.8660254037844389, -0.5, 0.8660254037844389, 1.0, 0});
  assert_cubic_roots(1, 1, 1, 1, (double[]){0.0, -1.0, 0.0, 1.0, -1.0, 0.0});
}
