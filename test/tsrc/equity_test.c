#include <assert.h>

#include "../../src/ent/equity.h"
#include "../../src/ent/move.h"

void test_equity(void) {
  // This ordering is required for movegen to work
  assert(EQUITY_INITIAL < EQUITY_PASS);
  // Equity -> double
  // Constants
  assert(within_epsilon_for_equity(equity_to_double(EQUITY_INITIAL),
                                   INITIAL_EQUITY_DOUBLE));
  assert(within_epsilon_for_equity(equity_to_double(EQUITY_PASS),
                                   PASS_EQUITY_DOUBLE));
  // Extremes
  assert(within_epsilon_for_equity(equity_to_double(MIN_EQUITY_VALUE),
                                   MIN_EQUITY_DOUBLE));
  assert(within_epsilon_for_equity(equity_to_double(MAX_EQUITY_VALUE),
                                   MAX_EQUITY_DOUBLE));
  // Midpoint
  assert(within_epsilon_for_equity(
      equity_to_double(MAX_EQUITY_VALUE / 2 + MIN_EQUITY_VALUE - 1),
      (MAX_EQUITY_DOUBLE - MIN_EQUITY_DOUBLE) / 2 + MIN_EQUITY_DOUBLE));

  // double -> Equity
  // Extremes
  assert(double_to_equity(MIN_EQUITY_DOUBLE) == MIN_EQUITY_VALUE);
  assert(double_to_equity(MAX_EQUITY_DOUBLE) == MAX_EQUITY_VALUE);
  // Midpoint
  assert(double_to_equity((MAX_EQUITY_DOUBLE - MIN_EQUITY_DOUBLE) / 2 +
                          MIN_EQUITY_DOUBLE) ==
         MAX_EQUITY_VALUE / 2 + MIN_EQUITY_VALUE - 1);

  // Brittle tests that depend on the value of MIN_EQUITY_DOUBLE and
  // MAX_EQUITY_DOUBLE.
  Equity eq_val = 2344200000;
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
}