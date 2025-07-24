#include <assert.h>
#include <stdint.h>

#include "../../src/def/equity_defs.h"

#include "../../src/ent/equity.h"

#include "test_util.h"

void test_stability_and_negation(Equity eq1) {
  if (eq1 == EQUITY_UNDEFINED_VALUE || eq1 == EQUITY_INITIAL_VALUE ||
      eq1 == EQUITY_PASS_VALUE) {
    return;
  }
  double dbl1 = equity_to_double(eq1);
  Equity eq2 = double_to_equity(dbl1);
  assert(eq1 == eq2);
  Equity eq_neg = equity_negate(eq1);
  Equity eq_neg_neg = equity_negate(eq_neg);
  assert(eq1 == eq_neg_neg);
}

void test_equity(void) {
  // This ordering is required for movegen to work
  assert(EQUITY_INITIAL_VALUE < EQUITY_PASS_VALUE);

  assert(int_to_equity(0) == 0);
  assert(int_to_equity(1) == EQUITY_RESOLUTION);
  assert(int_to_equity(-1) == -EQUITY_RESOLUTION);

  assert(within_epsilon(equity_to_double(0), 0.0));
  assert(within_epsilon(equity_to_double(EQUITY_MIN_VALUE), EQUITY_MIN_DOUBLE));
  assert(within_epsilon(equity_to_double(EQUITY_MAX_VALUE), EQUITY_MAX_DOUBLE));

  assert(double_to_equity(0.0) == 0);
  assert(double_to_equity(EQUITY_MIN_DOUBLE) == EQUITY_MIN_VALUE);
  assert(double_to_equity(EQUITY_MAX_DOUBLE) == EQUITY_MAX_VALUE);

  assert(double_to_equity(0.00000008) == 0);
  assert(double_to_equity(0.0000000008) == 0);
  assert(double_to_equity(0.00000000008) == 0);
  assert(double_to_equity(-0.00000008) == 0);
  assert(double_to_equity(-0.0000000008) == 0);
  assert(double_to_equity(-0.00000000008) == 0);

  // Check stability
  test_stability_and_negation(EQUITY_MIN_VALUE);
  test_stability_and_negation(EQUITY_MAX_VALUE);
  test_stability_and_negation(0);
  // Make eq_val an unsigned int to allow for overflow
  // to test a variety of different values.
  uint32_t eq_val = EQUITY_MIN_VALUE;
  for (int i = 0; i < 1000; i++) {
    test_stability_and_negation((Equity)eq_val);
    eq_val += 10000000;
  }
}