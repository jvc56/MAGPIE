#ifndef EQUITY_H
#define EQUITY_H

#include <math.h>
#include <stdint.h>

#include "../util/log.h"

typedef int32_t Equity;

#define EQUITY_RESOLUTION 10000
#define EQUITY_MIN_VALUE INT32_MIN
#define EQUITY_MAX_VALUE INT32_MAX
#define EQUITY_MIN_DOUBLE ((double)EQUITY_MIN_VALUE / EQUITY_RESOLUTION)
#define EQUITY_MAX_DOUBLE ((double)EQUITY_MAX_VALUE / EQUITY_RESOLUTION)
#define EQUITY_INITIAL_VALUE INT32_MIN
#define EQUITY_PASS_VALUE INT32_MIN + 1
#define EQUITY_ZERO_VALUE 0

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
  return (double)eq / EQUITY_RESOLUTION;
}

#endif
