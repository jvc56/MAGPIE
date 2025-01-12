#ifndef EQUITY_DEFS_H
#define EQUITY_DEFS_H

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

#endif