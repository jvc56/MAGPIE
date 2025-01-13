#ifndef EQUITY_DEFS_H
#define EQUITY_DEFS_H

#define EQUITY_UNDEFINED_VALUE INT32_MIN
#define EQUITY_INITIAL_VALUE (INT32_MIN + 1)
#define EQUITY_PASS_VALUE (INT32_MIN + 2)
#define EQUITY_ZERO_VALUE 0
// There are three reserved values at the bottom of the equity range
#define EQUITY_MIN_VALUE (INT32_MIN + 3)
// There is only one reserved value at the top of the equity range, but
// we negate EQUITY_MIN_VALUE to make the range symmetrical
#define EQUITY_MAX_VALUE (-EQUITY_MIN_VALUE)
#define EQUITY_RESOLUTION 100
#define EQUITY_MIN_DOUBLE ((double)EQUITY_MIN_VALUE / EQUITY_RESOLUTION)
#define EQUITY_MAX_DOUBLE ((double)EQUITY_MAX_VALUE / EQUITY_RESOLUTION)

#endif