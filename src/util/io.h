#ifndef IO_H
#define IO_H

#include <stdio.h>

typedef enum {
  LOG_TRACE,
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARN,
  LOG_FATAL,
} log_level_t;

#define log_trace(...) log_with_info(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_with_info(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...) log_with_info(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...) log_with_info(LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_with_info(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

void log_with_info(log_level_t log_level, const char *file, int line,
                   const char *fmt, ...);

void write_to_stream_out(const char *fmt, ...);
void write_to_stream_err(const char *fmt, ...);
void write_to_stream(FILE *stream, const char *fmt, ...);
char *read_line_from_stdin(void);

// WARNING: This function should only be called once at startup or for testing
void log_set_level(log_level_t new_log_level);

// WARNING: This function should only be used for testing
void io_set_stream_out(FILE *stream);

// WARNING: This function should only be used for testing
void io_reset_stream_out(void);

// WARNING: This function should only be used for testing
void io_set_stream_err(FILE *stream);

// WARNING: This function should only be used for testing
void io_reset_stream_err(void);

// WARNING: This function should only be used for testing
void io_set_stream_in(FILE *stream);

// WARNING: This function should only be used for testing
void io_reset_stream_in(void);

#endif