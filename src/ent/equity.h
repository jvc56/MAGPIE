#ifndef EQUITY_H
#define EQUITY_H

#include <stdint.h>

#include "../util/log.h"

typedef uint32_t Equity;

typedef enum {
  EQUITY_UNDEFINED,
  EQUITY_INITIAL,
  EQUITY_PASS,
  NUMBER_OF_RESERVED_EQUITY_CONSTANTS,
} equity_constants_t;

#define MIN_EQUITY_DOUBLE -200.0
#define MAX_EQUITY_DOUBLE 200.0
#define INITIAL_EQUITY_DOUBLE -10000.0
#define PASS_EQUITY_DOUBLE -1000.0
#define NUMBER_OF_AVAILABLE_EQUITIES                                           \
  (UINT32_MAX - NUMBER_OF_RESERVED_EQUITY_CONSTANTS)
#define MIN_EQUITY_VALUE NUMBER_OF_RESERVED_EQUITY_CONSTANTS
#define MAX_EQUITY_VALUE UINT32_MAX

Equity double_to_equity(double x) {
  if (x < MIN_EQUITY_DOUBLE) {
    log_fatal("Equity value below valid range: %f\n", x);
  } else if (x > MAX_EQUITY_DOUBLE) {
    log_fatal("Equity value above valid range: %f\n", x);
  }

  double range = MAX_EQUITY_DOUBLE - MIN_EQUITY_DOUBLE;
  Equity normalized_value =
      (Equity)((x - MIN_EQUITY_DOUBLE) / range * NUMBER_OF_AVAILABLE_EQUITIES +
               0.5);

  return normalized_value + NUMBER_OF_RESERVED_EQUITY_CONSTANTS;
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

  Equity normalized_value = x - NUMBER_OF_RESERVED_EQUITY_CONSTANTS;

  double range = MAX_EQUITY_DOUBLE - MIN_EQUITY_DOUBLE;
  return (double)normalized_value / NUMBER_OF_AVAILABLE_EQUITIES * range +
         MIN_EQUITY_DOUBLE;
}

#endif
