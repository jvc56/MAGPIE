// Unit test for sim_utility_blend: combines rollout win% and
// (sigmoid-normalized) spread into a single scalar BAI sample value.

#include "bai_utility_test.h"

#include "../src/ent/equity.h"
#include "../src/ent/sim_args.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void assert_close(double got, double expected, double tol,
                         const char *label) {
  if (fabs(got - expected) > tol) {
    printf("  FAIL %s: got %.9f, expected %.9f (tol %.9g)\n", label, got,
           expected, tol);
    assert(false);
  }
}

// Exact bit-equal compare. Used for the default-config short-circuit so we
// catch regressions if anyone later removes the `if (w_spread == 0) return
// wpct;` fast path and replaces it with FP arithmetic that happens to round
// to wpct.
static void assert_exact(double got, double expected, const char *label) {
  if (got != expected) {
    printf("  FAIL %s: got %.17g, expected %.17g (not bit-equal)\n", label, got,
           expected);
    assert(false);
  }
}

static double sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }

void test_bai_utility(void) {
  printf("\n=== bai_utility_test ===\n");
  const double tol = 1e-9;
  const Equity zero = int_to_equity(0);
  const Equity plus50 = int_to_equity(50);
  const Equity plus100 = int_to_equity(100);
  const Equity minus100 = int_to_equity(-100);
  const Equity plus200 = int_to_equity(200);
  const Equity minus200 = int_to_equity(-200);
  const Equity plus1000 = int_to_equity(1000);
  const Equity minus1000 = int_to_equity(-1000);

  // Default: (1, 0, 100) -- pure win%, byte-identical to wpct.
  assert_exact(sim_utility_blend(0.42, plus100, 1.0, 0.0, 100.0), 0.42,
               "default returns wpct exactly");
  assert_exact(sim_utility_blend(0.0, plus100, 1.0, 0.0, 100.0), 0.0,
               "default with wpct=0");
  assert_exact(sim_utility_blend(1.0, minus1000, 1.0, 0.0, 100.0), 1.0,
               "default ignores spread sign and magnitude");

  // Pure spread: (0, 1, 100) -- returns sigmoid(spread/scale).
  assert_close(sim_utility_blend(0.42, zero, 0.0, 1.0, 100.0), 0.5, tol,
               "zero spread => 0.5");
  assert_close(sim_utility_blend(0.42, plus50, 0.0, 1.0, 100.0), sigmoid(0.5),
               tol, "+50 => sigmoid(0.5) ~ 0.6225");
  assert_close(sim_utility_blend(0.42, plus100, 0.0, 1.0, 100.0), sigmoid(1.0),
               tol, "+100 => sigmoid(1.0) ~ 0.7311");
  assert_close(sim_utility_blend(0.42, minus100, 0.0, 1.0, 100.0),
               sigmoid(-1.0), tol, "-100 => sigmoid(-1.0) ~ 0.2689");
  assert_close(sim_utility_blend(0.42, plus200, 0.0, 1.0, 100.0), sigmoid(2.0),
               tol, "+200 => sigmoid(2.0) ~ 0.8808");
  assert_close(sim_utility_blend(0.42, minus200, 0.0, 1.0, 100.0),
               sigmoid(-2.0), tol, "-200 => sigmoid(-2.0) ~ 0.1192");
  // Extreme spreads: still strictly bounded, no longer clamped flat.
  assert_close(sim_utility_blend(0.42, plus1000, 0.0, 1.0, 100.0),
               sigmoid(10.0), tol, "+1000 => sigmoid(10) ~ 0.99995");
  assert_close(sim_utility_blend(0.42, minus1000, 0.0, 1.0, 100.0),
               sigmoid(-10.0), tol, "-1000 => sigmoid(-10) ~ 4.5e-5");

  // 50/50 blend at (wpct=0.6, spread=+50): (0.6 + sigmoid(0.5)) / 2.
  {
    const double expected = (0.6 + sigmoid(0.5)) / 2.0;
    assert_close(sim_utility_blend(0.6, plus50, 1.0, 1.0, 100.0), expected, tol,
                 "50/50 blend at (0.6, +50)");
  }

  // Weight normalization: (7, 3) == (0.7, 0.3) == (70, 30).
  assert_close(sim_utility_blend(0.6, plus50, 7.0, 3.0, 100.0),
               sim_utility_blend(0.6, plus50, 0.7, 0.3, 100.0), tol,
               "(7,3) == (0.7,0.3)");
  assert_close(sim_utility_blend(0.6, plus50, 70.0, 30.0, 100.0),
               sim_utility_blend(0.6, plus50, 0.7, 0.3, 100.0), tol,
               "(70,30) == (0.7,0.3)");

  // Custom spread scale: scale=50 with spread=+25 -> sigmoid(0.5).
  assert_close(sim_utility_blend(0.0, int_to_equity(25), 0.0, 1.0, 50.0),
               sigmoid(0.5), tol, "scale=50 with +25 -> sigmoid(0.5)");

  // Pure-spread term is strictly monotonic increasing in the spread value
  // -- this is the property the clamp implementation lacked beyond +/- scale.
  double prev_u = -1.0;
  for (int s = -500; s <= 500; s += 25) {
    const double u = sim_utility_blend(0.5, int_to_equity(s), 0.0, 1.0, 100.0);
    if (u <= prev_u) {
      printf("  FAIL monotonic: u(%d)=%.9f <= prev=%.9f\n", s, u, prev_u);
      assert(false);
    }
    prev_u = u;
  }

  // Output is always in [0, 1] regardless of inputs.
  for (int s = -2000; s <= 2000; s += 100) {
    for (double w = 0.0; w <= 1.0; w += 0.1) {
      const double u = sim_utility_blend(w, int_to_equity(s), 1.0, 1.0, 100.0);
      assert(u >= 0.0);
      assert(u <= 1.0);
    }
  }

  printf("  PASSED\n");
}
