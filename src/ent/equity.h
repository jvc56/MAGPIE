#ifndef EQUITY_H
#define EQUITY_H

#include "../def/equity_defs.h"
#include "../util/io_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef int32_t Equity;

// Used when loading KLV data and in tests.
static inline Equity double_to_equity(double x) {
  if (x > EQUITY_MAX_DOUBLE || x < EQUITY_MIN_DOUBLE) {
    log_fatal("equity value out of range: %f", x);
  }
  double rounded = round(x * EQUITY_RESOLUTION);
  if (rounded > EQUITY_MAX_VALUE) {
    return EQUITY_MAX_VALUE;
  }
  if (rounded < EQUITY_MIN_VALUE) {
    return EQUITY_MIN_VALUE;
  }
  return (Equity)rounded;
}

// Used for tests and accumulating stats.
static inline double equity_to_double(Equity eq) {
  if (eq == EQUITY_UNDEFINED_VALUE) {
    log_fatal("cannot convert undefined equity");
  }
  if (eq == EQUITY_INITIAL_VALUE) {
    log_fatal("cannot convert initial equity");
  }
  if (eq == EQUITY_PASS_VALUE) {
    log_fatal("cannot convert pass equity");
  }
  return (double)eq / EQUITY_RESOLUTION;
}

static inline Equity equity_negate(Equity eq) {
  if (eq == EQUITY_UNDEFINED_VALUE) {
    log_fatal("cannot negate undefined equity");
  }
  if (eq == EQUITY_INITIAL_VALUE) {
    log_fatal("cannot negate initial equity");
  }
  if (eq == EQUITY_PASS_VALUE) {
    log_fatal("cannot negate pass equity");
  }

  // EQUITY_UNDEFINED_VALUE is INT32_MIN and so negation *would* be an overflow,
  // but this is caught by the check above.
  // cppcheck-suppress integerOverflowCond
  return -eq;
}

static inline bool equity_is_integer(Equity eq) {
  return eq % EQUITY_RESOLUTION == 0;
}

// Mostly used for tests and display, but SmallMove (for endgame) converts
// scores to regular integers. We could consider making a faster unsafe version
// of this to use for that.
static inline int equity_to_int(Equity eq) {
  if (eq == EQUITY_UNDEFINED_VALUE) {
    log_fatal("cannot convert undefined equity");
  }
  if (eq == EQUITY_INITIAL_VALUE) {
    log_fatal("cannot convert initial equity");
  }
  if (eq == EQUITY_PASS_VALUE) {
    log_fatal("cannot convert pass equity");
  }
  if (!equity_is_integer(eq)) {
    log_fatal("equity is not an integer");
  }
  return eq / EQUITY_RESOLUTION;
}

// Should only be needed for tests and loading data, not in movegen etc.
static inline Equity int_to_equity(int x) {
  if (x > EQUITY_MAX_DOUBLE || x < EQUITY_MIN_DOUBLE) {
    log_fatal("equity value out of range: %f", x);
  }
  return x * EQUITY_RESOLUTION;
}

#endif
