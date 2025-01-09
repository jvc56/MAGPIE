#include <assert.h>

#include "../../src/ent/equity.h"
#include "../../src/ent/move.h"

#include "test_util.h"

void test_stability(Equity eq1) {
  double dbl1 = equity_to_double(eq1);
  Equity eq2 = double_to_equity(dbl1);
  assert(eq1 == eq2);
}

void test_equity(void) {
  // This ordering is required for movegen to work
  assert(EQUITY_INITIAL_VALUE < EQUITY_PASS_VALUE);

  assert(within_epsilon(equity_to_double(EQUITY_ZERO_VALUE), 0.0));
  assert(within_epsilon(equity_to_double(EQUITY_MIN_VALUE), EQUITY_MIN_DOUBLE));
  assert(within_epsilon(equity_to_double(EQUITY_MAX_VALUE), EQUITY_MAX_DOUBLE));

  assert(double_to_equity(0.0) == EQUITY_ZERO_VALUE);
  assert(double_to_equity(EQUITY_MIN_DOUBLE) == EQUITY_MIN_VALUE);
  assert(double_to_equity(EQUITY_MAX_DOUBLE) == EQUITY_MAX_VALUE);

  assert(double_to_equity(0.00000008) == EQUITY_ZERO_VALUE);
  assert(double_to_equity(0.0000000008) == EQUITY_ZERO_VALUE);
  assert(double_to_equity(0.00000000008) == EQUITY_ZERO_VALUE);
  assert(double_to_equity(-0.00000008) == EQUITY_ZERO_VALUE);
  assert(double_to_equity(-0.0000000008) == EQUITY_ZERO_VALUE);
  assert(double_to_equity(-0.00000000008) == EQUITY_ZERO_VALUE);

  // Check stability
  test_stability(EQUITY_MIN_VALUE);
  test_stability(EQUITY_MAX_VALUE);
  test_stability(EQUITY_ZERO_VALUE);
  // Make eq_val an unsigned int to allow for overflow
  // to test a variety of different values.
  uint32_t eq_val = EQUITY_MIN_VALUE;
  for (int i = 0; i < 1000; i++) {
    test_stability((Equity)eq_val);
    eq_val += 10000000;
  }
}