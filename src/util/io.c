#include "io.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static log_level_t current_log_level = LOG_FATAL;
static FILE *stream_out = NULL;
static FILE *stream_err = NULL;
static FILE *stream_in = NULL;

void log_set_level(log_level_t new_log_level) {
  current_log_level = new_log_level;
}
void io_set_stream_out(FILE *stream) { stream_out = stream; }
void io_set_stream_err(FILE *stream) { stream_err = stream; }
void io_set_stream_in(FILE *stream) { stream_in = stream; }

FILE *get_stream_out(void) {
  FILE *stream = stream_out;
  if (!stream) {
    stream = stdout;
  }
  return stream;
}

FILE *get_stream_err(void) {
  FILE *stream = stream_err;
  if (!stream) {
    stream = stderr;
  }
  return stream;
}

FILE *get_stream_in(void) {
  FILE *stream = stream_in;
  if (!stream) {
    stream = stdin;
  }
  return stream;
}

void vwrite_to_stream(FILE *stream, const char *fmt, va_list args) {
  vfprintf(stream, fmt, args);
  fflush(stream);
}

void log(log_level_t log_level, const char *caller_filename, int callerline,
         const char *format, ...) {
  if (log_level < current_log_level) {
    return;
  }
  FILE *output_fh = NULL;
  const char *level_string = NULL;
  bool exit_fatally = false;
  switch (log_level) {
  case LOG_TRACE:
    output_fh = get_stream_out();
    level_string = "TRACE";
    break;
  case LOG_DEBUG:
    output_fh = get_stream_out();
    level_string = "DEBUG";
    break;
  case LOG_INFO:
    output_fh = get_stream_out();
    level_string = "INFO";
    break;
  case LOG_WARN:
    output_fh = get_stream_err();
    level_string = "WARN";
    break;
  case LOG_FATAL:
    output_fh = get_stream_err();
    level_string = "FATAL";
    exit_fatally = true;
    break;
  }

  char time_buf[64];
  time_t t = time(NULL);
  time_buf[strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                    localtime(&t))] = '\0';
  fprintf(output_fh, "[%s] %-5s %s:%d: ", time_buf, level_string,
          caller_filename, callerline);
  va_list args;
  va_start(args, format);
  vwrite_to_stream(output_fh, format, args);
  va_end(args);

  if (exit_fatally) {
    abort();
  }
}

void write_to_stdout(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vwrite_to_stream(get_stream_out(), fmt, args);
  va_end(args);
}

void write_to_stderr(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vwrite_to_stream(get_stream_in(), fmt, args);
  va_end(args);
}

void write_to_stream(FILE *stream, const char *fmt, ...) {
  assert(stream);
  va_list args;
  va_start(args, fmt);
  vwrite_to_stream(stream, fmt, args);
  va_end(args);
}

char *read_line_from_stdin() {
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  errno = 0;
  read = getline(&line, &len, get_stream_in());
  if (read == -1) {
    int error_number = errno;
    if (error_number) {
      log_fatal("failed to read from stdin with error: %d\n", error_number);
    } else {
      free(line);
      line = NULL;
    }
  }
  if (read && read > 0 && line[read - 1] == '\n') {
    line[read - 1] = '\0';
  }
  return line;
}