#ifndef CFORMAT_H
#define CFORMAT_H

// Marks a function as taking a printf format, so that -Wformat checks the
// arguments every caller passes against it: the format string is parameter
// format_idx (counting from 1) and the arguments start at first_arg_idx, or
// 0 for a function that takes a va_list.
#if defined(__MINGW32__) && !defined(__clang__)
// MinGW GCC checks the printf archetype against Microsoft's old printf
// unless it is told the runtime is C99's, as UCRT's is.
#define CFORMAT_PRINTF(format_idx, first_arg_idx)                              \
  __attribute__((format(gnu_printf, format_idx, first_arg_idx)))
#elif defined(__GNUC__)
#define CFORMAT_PRINTF(format_idx, first_arg_idx)                              \
  __attribute__((format(printf, format_idx, first_arg_idx)))
#else
#define CFORMAT_PRINTF(format_idx, first_arg_idx)
#endif

#endif
