#ifndef EQUITY_H
#define EQUITY_H

#include <math.h>
#include <stdint.h>

#include "../util/log.h"

typedef uint32_t Equity;

typedef enum {
  EQUITY_UNDEFINED,
  EQUITY_INITIAL,
  EQUITY_PASS,
  EQUITY_ZERO,
  NUMBER_OF_RESERVED_EQUITY_VALUES,
} equity_constants_t;

#define MIN_EQUITY_DOUBLE -200.0
#define MAX_EQUITY_DOUBLE 200.0
#define EQUITY_RANGE_DOUBLE (MAX_EQUITY_DOUBLE - MIN_EQUITY_DOUBLE)
#define INITIAL_EQUITY_DOUBLE -10000.0
#define PASS_EQUITY_DOUBLE -1000.0
#define NUMBER_OF_AVAILABLE_EQUITY_VALUES                                      \
  (UINT32_MAX - NUMBER_OF_RESERVED_EQUITY_VALUES)
#define EQUITY_RESOLUTION                                                      \
  (EQUITY_RANGE_DOUBLE / (double)NUMBER_OF_AVAILABLE_EQUITY_VALUES)
#define MIN_EQUITY_VALUE NUMBER_OF_RESERVED_EQUITY_VALUES
#define MAX_EQUITY_VALUE UINT32_MAX

Equity double_to_equity(double x) {
  if (x < MIN_EQUITY_DOUBLE) {
    log_fatal("Equity value below valid range: %f\n", x);
  } else if (x > MAX_EQUITY_DOUBLE) {
    log_fatal("Equity value above valid range: %f\n", x);
  }
  if (fabs(x) < EQUITY_RESOLUTION) {
    return EQUITY_ZERO;
  }
  return (Equity)((x - MIN_EQUITY_DOUBLE) / EQUITY_RANGE_DOUBLE *
                      NUMBER_OF_AVAILABLE_EQUITY_VALUES +
                  0.5) +
         NUMBER_OF_RESERVED_EQUITY_VALUES;
}

double equity_to_double(Equity x) {
  if (x == EQUITY_UNDEFINED) {
    log_fatal("Attempted to convert undefined equity to double\n");
  }
  if (x == EQUITY_INITIAL) {
    return INITIAL_EQUITY_DOUBLE;
  }
  if (x == EQUITY_PASS) {
    return PASS_EQUITY_DOUBLE;
  }
  if (x == EQUITY_ZERO) {
    return 0.0;
  }
  return (double)(x - NUMBER_OF_RESERVED_EQUITY_VALUES) /
             NUMBER_OF_AVAILABLE_EQUITY_VALUES * EQUITY_RANGE_DOUBLE +
         MIN_EQUITY_DOUBLE;
}

#endif
