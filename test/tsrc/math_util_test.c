#include <assert.h>
#include <math.h>

#include "../../src/def/math_util_defs.h"
#include "../../src/impl/bai_peps.h"

#include "../../src/util/math_util.h"

#include "test_util.h"

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
}
