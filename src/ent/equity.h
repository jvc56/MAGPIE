#ifndef EQUITY_H
#define EQUITY_H

#include <assert.h>
#include <math.h>
#include <stdint.h>

#include "../def/equity_defs.h"

#include "../util/log.h"

typedef int32_t Equity;

static inline Equity double_to_equity(double x) {
  if (x > EQUITY_MAX_DOUBLE || x < EQUITY_MIN_DOUBLE) {
    log_fatal("equity value out of range: %f", x);
  }
  double rounded = round(x * EQUITY_RESOLUTION);
  if (rounded > EQUITY_MAX_VALUE) {
    return EQUITY_MAX_VALUE;
  } else if (rounded < EQUITY_MIN_VALUE) {
    return EQUITY_MIN_VALUE;
  }
  return (Equity)rounded;
}

static inline double equity_to_double(Equity eq) {
  if (eq == EQUITY_UNDEFINED_VALUE) {
    log_fatal("cannot convert undefined equity\n");
  }
  if (eq == EQUITY_INITIAL_VALUE) {
    log_fatal("cannot convert initial equity\n");
  }
  if (eq == EQUITY_PASS_VALUE) {
    log_fatal("cannot convert pass equity");
  }
  return (double)eq / EQUITY_RESOLUTION;
}

static inline Equity equity_negate(Equity eq) {
  if (eq == EQUITY_UNDEFINED_VALUE) {
    log_fatal("cannot negate undefined equity\n");
  }
  if (eq == EQUITY_INITIAL_VALUE) {
    log_fatal("cannot negate initial equity\n");
  }
  if (eq == EQUITY_PASS_VALUE) {
    log_fatal("cannot negate pass equity\n");
  }
  return -eq;
}

static inline bool equity_is_integer(Equity eq) {
  return eq % EQUITY_RESOLUTION == 0;
}

// Should only be used for tests and display.
static inline int equity_to_int(Equity eq) {
  if (eq == EQUITY_UNDEFINED_VALUE) {
    log_fatal("cannot convert undefined equity\n");
  }
  if (eq == EQUITY_INITIAL_VALUE) {
    log_fatal("cannot convert initial equity\n");
  }
  if (eq == EQUITY_PASS_VALUE) {
    log_fatal("cannot convert pass equity\n");
  }
  if (!equity_is_integer(eq)) {
    log_fatal("equity is not an integer\n");
  }
  return eq / EQUITY_RESOLUTION;
}

// Should only be needed for tests and loading data, not in movegen etc.
static inline Equity int_to_equity(int x) {
  if (x > EQUITY_MAX_DOUBLE || x < EQUITY_MIN_DOUBLE) {
    log_fatal("equity value out of range: %f", x);
  }
  const int64_t multiplied = x * EQUITY_RESOLUTION;
  return (Equity)multiplied;
}


#endif
