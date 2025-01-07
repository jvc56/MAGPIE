#include <assert.h>

#include "../../src/ent/equity.h"
#include "../../src/ent/move.h"

void test_stability(Equity eq1) {
  double dbl1 = equity_to_double(eq1);
  Equity eq2 = double_to_equity(dbl1);
  assert(eq1 == eq2);
}

void test_equity(void) {
  // This ordering is required for movegen to work
  assert(EQUITY_INITIAL < EQUITY_PASS);
  // Equity -> double
  // Constants
  assert(within_epsilon_for_equity(equity_to_double(EQUITY_INITIAL),
                                   INITIAL_EQUITY_DOUBLE));
  assert(within_epsilon_for_equity(equity_to_double(EQUITY_PASS),
                                   PASS_EQUITY_DOUBLE));
  assert(within_epsilon_for_equity(equity_to_double(EQUITY_ZERO), 0.0));
  // Extremes
  assert(within_epsilon_for_equity(equity_to_double(MIN_EQUITY_VALUE),
                                   MIN_EQUITY_DOUBLE));
  assert(within_epsilon_for_equity(equity_to_double(MAX_EQUITY_VALUE),
                                   MAX_EQUITY_DOUBLE));

  // double -> Equity
  // Extremes
  assert(double_to_equity(MIN_EQUITY_DOUBLE) == MIN_EQUITY_VALUE);
  assert(double_to_equity(MAX_EQUITY_DOUBLE) == MAX_EQUITY_VALUE);
  assert(double_to_equity(0.0) == EQUITY_ZERO);

  // Check stability
  test_stability(MIN_EQUITY_VALUE);
  test_stability(MAX_EQUITY_VALUE);
  test_stability(EQUITY_ZERO);
  Equity eq_val = MIN_EQUITY_VALUE;
  for (int i = 0; i < 1000; i++) {
    test_stability(eq_val);
    eq_val += 10000000;
  }

  // Brittle tests that depend on the value of MIN_EQUITY_DOUBLE and
  // MAX_EQUITY_DOUBLE.
  eq_val = 2344200000;
  double eq_dbl = 18.3206379922;
  assert(within_epsilon_for_equity(equity_to_double(eq_val), eq_dbl));
  eq_val = 123456789;
  eq_dbl = -188.502190539;
  assert(within_epsilon_for_equity(equity_to_double(eq_val), eq_dbl));
  eq_val = 2040109467;
  eq_dbl = -10.0;
  assert(double_to_equity(eq_dbl) == eq_val);
  eq_val = 2469606196;
  eq_dbl = 30.0;
  assert(double_to_equity(eq_dbl) == eq_val);

  assert(double_to_equity(0.00000008) == EQUITY_ZERO);
  assert(double_to_equity(0.0000000008) == EQUITY_ZERO);
  assert(double_to_equity(0.00000000008) == EQUITY_ZERO);
  assert(double_to_equity(-0.00000008) == EQUITY_ZERO);
  assert(double_to_equity(-0.0000000008) == EQUITY_ZERO);
  assert(double_to_equity(-0.00000000008) == EQUITY_ZERO);
}