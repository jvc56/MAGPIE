// Unit test for sim_utility_blend: the helper that combines rollout win%
// and (normalized) spread into a single scalar BAI sample value.

#include "bai_utility_test.h"

#include "../src/ent/equity.h"
#include "../src/ent/sim_args.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void assert_close(double got, double expected, double tol,
                         const char *label) {
  if (fabs(got - expected) > tol) {
    printf("  FAIL %s: got %.6f, expected %.6f (tol %.6f)\n", label, got,
           expected, tol);
    assert(false);
  }
}

void test_bai_utility(void) {
  printf("\n=== bai_utility_test ===\n");
  const double tol = 1e-9;
  const Equity zero = int_to_equity(0);
  const Equity plus100 = int_to_equity(100);
  const Equity minus100 = int_to_equity(-100);
  const Equity plus50 = int_to_equity(50);
  const Equity plus200 = int_to_equity(200);
  const Equity minus200 = int_to_equity(-200);

  // Default: (1, 0, 100) — pure win%, byte-identical to wpct.
  assert_close(sim_utility_blend(0.42, plus100, 1.0, 0.0, 100.0), 0.42, tol,
               "default returns wpct exactly");
  assert_close(sim_utility_blend(0.0, plus100, 1.0, 0.0, 100.0), 0.0, tol,
               "default with wpct=0");
  assert_close(sim_utility_blend(1.0, minus200, 1.0, 0.0, 100.0), 1.0, tol,
               "default ignores spread sign and magnitude");

  // Pure spread: (0, 1, 100) — returns spread01 in [0, 1].
  assert_close(sim_utility_blend(0.42, zero, 0.0, 1.0, 100.0), 0.5, tol,
               "zero spread => 0.5");
  assert_close(sim_utility_blend(0.42, plus100, 0.0, 1.0, 100.0), 1.0, tol,
               "+100 saturates to 1.0");
  assert_close(sim_utility_blend(0.42, minus100, 0.0, 1.0, 100.0), 0.0, tol,
               "-100 saturates to 0.0");
  assert_close(sim_utility_blend(0.42, plus50, 0.0, 1.0, 100.0), 0.75, tol,
               "+50/100 => 0.75");
  assert_close(sim_utility_blend(0.42, plus200, 0.0, 1.0, 100.0), 1.0, tol,
               "+200 clamps to +1 then 1.0");
  assert_close(sim_utility_blend(0.42, minus200, 0.0, 1.0, 100.0), 0.0, tol,
               "-200 clamps to -1 then 0.0");

  // 50/50 blend.
  // wpct = 0.6, spread = +50 -> spread01 = 0.75
  // utility = (1*0.6 + 1*0.75) / 2 = 0.675
  assert_close(sim_utility_blend(0.6, plus50, 1.0, 1.0, 100.0), 0.675, tol,
               "50/50 blend at (0.6, +50)");

  // Unnormalized weights produce same answer as normalized.
  assert_close(sim_utility_blend(0.6, plus50, 7.0, 3.0, 100.0),
               sim_utility_blend(0.6, plus50, 0.7, 0.3, 100.0), tol,
               "weights normalize themselves");
  assert_close(sim_utility_blend(0.6, plus50, 70.0, 30.0, 100.0),
               sim_utility_blend(0.6, plus50, 0.7, 0.3, 100.0), tol,
               "scaling both weights by 100 doesn't change result");

  // Custom spread scale.
  // scale=50 means +25 = +0.5 normalized -> spread01 = 0.75
  assert_close(sim_utility_blend(0.0, int_to_equity(25), 0.0, 1.0, 50.0), 0.75,
               tol, "spread_scale=50 with +25 -> 0.75");

  // Output is always in [0, 1] regardless of inputs.
  for (int s = -300; s <= 300; s += 25) {
    for (double w = 0.0; w <= 1.0; w += 0.1) {
      const double u = sim_utility_blend(w, int_to_equity(s), 1.0, 1.0, 100.0);
      assert(u >= 0.0 - tol);
      assert(u <= 1.0 + tol);
    }
  }

  printf("  PASSED\n");
}
