#ifndef EQUITY_H
#define EQUITY_H

#include <math.h>
#include <stdint.h>

#include "../util/log.h"

typedef int32_t Equity;

#define EQUITY_UNDEFINED_VALUE INT32_MIN
#define EQUITY_INITIAL_VALUE (INT32_MIN + 1)
#define EQUITY_PASS_VALUE INT32_MAX
#define EQUITY_ZERO_VALUE 0
// There are two reserved values at the bottom of the equity range
#define EQUITY_MIN_VALUE (INT32_MIN + 2)
// There is one reserved value at the top of the equity range
#define EQUITY_MAX_VALUE (INT32_MAX - 1)
#define EQUITY_RESOLUTION 1000
#define EQUITY_MIN_DOUBLE ((double)EQUITY_MIN_VALUE / EQUITY_RESOLUTION)
#define EQUITY_MAX_DOUBLE ((double)EQUITY_MAX_VALUE / EQUITY_RESOLUTION)
#define EQUITY_PASS_DOUBLE -1000000.0

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
    return EQUITY_PASS_DOUBLE;
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

#endif
