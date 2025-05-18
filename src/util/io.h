#ifndef IO_H
#define IO_H

typedef enum {
  LOG_TRACE,
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARN,
  LOG_FATAL,
} log_level_t;

#define log_trace(...) (LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) (LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...) (LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...) (LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) (LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

void log(log_level_t log_level, const char *file, int line, const char *fmt,
         ...);
void log_set_level(log_level_t new_log_level);
void write_to_stdout(const char *fmt, ...);
void write_to_stderr(const char *fmt, ...);
void write_to_stream(FILE *stream, const char *fmt, ...);
char *read_line_from_stdin();
void log_set_level(log_level_t new_log_level);
void io_set_stream_out(FILE *stream);
void io_set_stream_err(FILE *stream);
void io_set_stream_in(FILE *stream);

#endif