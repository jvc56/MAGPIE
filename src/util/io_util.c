#include "io_util.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ERROR_STACK_CAPACITY = 100 };

static log_level_t current_log_level = LOG_FATAL;
static FILE *stream_out = NULL;
static FILE *stream_err = NULL;
static FILE *stream_in = NULL;

// pthread.h is included above (line 5) and is the correct public API header
// for pthread_mutex_t. The type is provided transitively through internal
// headers, which is standard POSIX pthread design. We use pthread directly
// here instead of cpthread wrappers to avoid circular dependency between
// cpthread.h (which includes io_util.h) and io_util.c.
// NOLINTNEXTLINE(misc-include-cleaner)
static pthread_mutex_t stream_mutex = PTHREAD_MUTEX_INITIALIZER;

FILE *get_stream_out(void) {
  pthread_mutex_lock(&stream_mutex);
  FILE *stream = stream_out;
  if (!stream) {
    stream = stdout;
  }
  pthread_mutex_unlock(&stream_mutex);
  return stream;
}

FILE *get_stream_err(void) {
  pthread_mutex_lock(&stream_mutex);
  FILE *stream = stream_err;
  if (!stream) {
    stream = stderr;
  }
  pthread_mutex_unlock(&stream_mutex);
  return stream;
}

FILE *get_stream_in(void) {
  pthread_mutex_lock(&stream_mutex);
  FILE *stream = stream_in;
  if (!stream) {
    stream = stdin;
  }
  pthread_mutex_unlock(&stream_mutex);
  return stream;
}

char *format_string_with_va_list(const char *format, va_list *args) {
  int size;
  va_list args_copy_for_size;
  va_copy(args_copy_for_size, *args);
  size = vsnprintf(NULL, 0, format, args_copy_for_size);
  if (size < 0) {
    log_fatal("vsnprintf failed to determine size for format: %s", format);
  }
  va_end(args_copy_for_size);
  size++;
  char *string_buffer = malloc_or_die(size);
  int bytes_written = vsnprintf(string_buffer, size, format, *args);
  if (bytes_written < 0) {
    log_fatal("vsnprintf failed when writing formatted string for format: %s",
              format);
  }
  if (bytes_written >= size) {
    log_fatal("vsnprintf string overflow when writing formatted string for "
              "format: %s",
              format);
  }
  return string_buffer;
}

char *get_formatted_string(const char *format, ...) {
  va_list args;
  va_start(args, format);
  char *formatted_string = format_string_with_va_list(format, &args);
  va_end(args);
  return formatted_string;
}

void fflush_or_die(FILE *stream) {
  if (fflush(stream) != 0) {
    int error_number = errno;
    const char *system_error_message = strerror(error_number);
    log_fatal("failed to flush stream: %s (%d)", system_error_message,
              error_number);
  }
}

void write_to_stream_with_vargs(FILE *stream, const bool flush, const char *fmt,
                                va_list args) {
  int bytes_written = vfprintf(stream, fmt, args);
  if (bytes_written < 0) {
    abort();
  }
  if (flush) {
    fflush_or_die(stream);
  }
}

void fprintf_or_die(FILE *stream, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int result = vfprintf(stream, format, args);
  va_end(args);
  if (result < 0) {
    abort();
  }
}

void log_with_info(log_level_t log_level, const char *caller_filename,
                   int caller_line, const char *format, ...) {
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
    output_fh = get_stream_out();
    level_string = "WARN";
    break;
  case LOG_FATAL:
    output_fh = get_stream_err();
    level_string = "FATAL";
    exit_fatally = true;
    break;
  }

  char time_buf[64];
  time_t current_time = time(NULL);
  time_buf[strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                    localtime(&current_time))] = '\0';
  fprintf_or_die(output_fh, "[%s] %-5s %s:%d: ", time_buf, level_string,
                 caller_filename, caller_line);
  va_list args;
  va_start(args, format);
  write_to_stream_with_vargs(output_fh, false, format, args);
  va_end(args);
  fprintf_or_die(output_fh, "\n");
  fflush_or_die(output_fh);

  if (exit_fatally) {
    abort();
  }
}

void write_to_stream_out(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  write_to_stream_with_vargs(get_stream_out(), true, fmt, args);
  va_end(args);
}

void write_to_stream_err(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  write_to_stream_with_vargs(get_stream_err(), true, fmt, args);
  va_end(args);
}

void write_to_stream(FILE *stream, const char *fmt, ...) {
  assert(stream);
  va_list args;
  va_start(args, fmt);
  write_to_stream_with_vargs(stream, true, fmt, args);
  va_end(args);
}

char *read_line_from_stream_in(void) {
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  errno = 0;
  read = getline(&line, &len, get_stream_in());
  if (read == -1) {
    int error_number = errno;
    if (error_number) {
      perror("error");
      log_fatal("failed to read from input stream with error: %d",
                error_number);
    } else {
      free(line);
      line = NULL;
    }
  }
  if (read > 0 && line[read - 1] == '\n') {
    line[read - 1] = '\0';
  }
  return line;
}

// WARNING: This function should only be called once at startup or for testing
void log_set_level(log_level_t new_log_level) {
  current_log_level = new_log_level;
}

// WARNING: This function should only be used for testing
void io_set_stream_out(FILE *stream) {
  pthread_mutex_lock(&stream_mutex);
  stream_out = stream;
  pthread_mutex_unlock(&stream_mutex);
}

// WARNING: This function should only be used for testing
void io_reset_stream_out(void) {
  pthread_mutex_lock(&stream_mutex);
  stream_out = NULL;
  pthread_mutex_unlock(&stream_mutex);
}

// WARNING: This function should only be used for testing
void io_set_stream_err(FILE *stream) {
  pthread_mutex_lock(&stream_mutex);
  stream_err = stream;
  pthread_mutex_unlock(&stream_mutex);
}

// WARNING: This function should only be used for testing
void io_reset_stream_err(void) {
  pthread_mutex_lock(&stream_mutex);
  stream_err = NULL;
  pthread_mutex_unlock(&stream_mutex);
}

// WARNING: This function should only be used for testing
void io_set_stream_in(FILE *stream) {
  pthread_mutex_lock(&stream_mutex);
  stream_in = stream;
  pthread_mutex_unlock(&stream_mutex);
}

// WARNING: This function should only be used for testing
void io_reset_stream_in(void) {
  pthread_mutex_lock(&stream_mutex);
  stream_in = NULL;
  pthread_mutex_unlock(&stream_mutex);
}

void *malloc_or_die(size_t size) {
  void *uncasted_pointer = malloc(size);
  if (!uncasted_pointer) {
    log_fatal("failed to malloc size of %lu", size);
  }
  return uncasted_pointer;
}

void *calloc_or_die(size_t number_of_elements, size_t size_of_element) {
  void *uncasted_pointer = calloc(number_of_elements, size_of_element);
  if (!uncasted_pointer) {
    log_fatal("failed to calloc %lu elements of size %lu", number_of_elements,
              size_of_element);
  }
  return uncasted_pointer;
}

void *realloc_or_die(void *realloc_target, size_t size) {
  void *realloc_result = realloc(realloc_target, size);
  if (!realloc_result) {
    log_fatal("failed to realloc %p with size of %lu", realloc_target, size);
  }
  return realloc_result;
}

struct ErrorStack {
  int size;
  error_code_t error_codes[ERROR_STACK_CAPACITY];
  char *msgs[ERROR_STACK_CAPACITY];
};

ErrorStack *error_stack_create(void) {
  ErrorStack *error_stack = malloc_or_die(sizeof(ErrorStack));
  error_stack->size = 0;
  for (int i = 0; i < ERROR_STACK_CAPACITY; i++) {
    error_stack->error_codes[i] = ERROR_STATUS_SUCCESS;
    error_stack->msgs[i] = NULL;
  }
  return error_stack;
}

void error_stack_reset(ErrorStack *error_stack) {
  error_stack->size = 0;
  for (int i = 0; i < ERROR_STACK_CAPACITY; i++) {
    error_stack->error_codes[i] = ERROR_STATUS_SUCCESS;
    free(error_stack->msgs[i]);
    error_stack->msgs[i] = NULL;
  }
}

void error_stack_destroy(ErrorStack *error_stack) {
  if (!error_stack) {
    return;
  }
  for (int i = 0; i < ERROR_STACK_CAPACITY; i++) {
    free(error_stack->msgs[i]);
  }
  free(error_stack);
}

// Takes ownership of the msg
void error_stack_push(ErrorStack *error_stack, error_code_t error_code,
                      char *msg) {
  if (error_stack->size == ERROR_STACK_CAPACITY) {
    log_fatal("error stack is full");
  }
  error_stack->error_codes[error_stack->size] = error_code;
  error_stack->msgs[error_stack->size] = msg;
  error_stack->size++;
}

bool error_stack_is_empty(const ErrorStack *error_stack) {
  return error_stack->size == 0;
}

error_code_t error_stack_top(ErrorStack *error_stack) {
  if (error_stack_is_empty(error_stack)) {
    return ERROR_STATUS_SUCCESS;
  }
  return error_stack->error_codes[error_stack->size - 1];
}

char *error_stack_get_string_and_reset(ErrorStack *error_stack) {
  if (error_stack_is_empty(error_stack)) {
    return NULL;
  }

  const char *error_fmt = "(error %d) %s\n";
  int total_length = 0;
  for (int i = 0; i < error_stack->size; i++) {
    total_length += snprintf(NULL, 0, error_fmt, error_stack->error_codes[i],
                             error_stack->msgs[i]);
  }

  // Use +1 for the null terminator
  char *error_string = malloc_or_die(total_length + 1);
  int offset = 0;
  for (int i = error_stack->size - 1; i >= 0; i--) {
    offset +=
        snprintf(error_string + offset, total_length + 1 - offset, error_fmt,
                 error_stack->error_codes[i], error_stack->msgs[i]);
  }
  error_string[total_length] = '\0';

  error_stack_reset(error_stack);
  return error_string;
}

void error_stack_print_and_reset(ErrorStack *error_stack) {
  char *error_string = error_stack_get_string_and_reset(error_stack);
  if (!error_string) {
    return;
  }
  write_to_stream_err(error_string);
  free(error_string);
}

void write_string_to_file(const char *filename, const char *mode,
                          const char *string, ErrorStack *error_stack) {
  FILE *file_handle = fopen_safe(filename, mode, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Write string to file
  if (fputs(string, file_handle) == EOF) {
    fclose_or_die(file_handle);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_WRITE_ERROR,
        get_formatted_string("error writing to file: %s", filename));
    return;
  }

  // Close the file handle
  fclose_or_die(file_handle);
}

void fseek_or_die(FILE *stream, long offset, int whence) {
  if (fseek(stream, offset, whence) != 0) {
    int error_number = errno;
    const char *system_error_message = strerror(error_number);
    log_fatal("failed to fseek stream: %s (%d)", system_error_message,
              error_number);
  }
}

char *get_string_from_file(const char *filename, ErrorStack *error_stack) {
  FILE *file_handle = fopen_safe(filename, "r", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  // Get the file size by seeking to the end and then back to the beginning
  fseek_or_die(file_handle, 0, SEEK_END);
  long file_size = ftell(file_handle);
  fseek_or_die(file_handle, 0, SEEK_SET);

  char *result_string =
      (char *)malloc_or_die(file_size + 1); // +1 for null terminator
  if (!result_string) {
    fclose_or_die(file_handle);
    free(result_string);
    error_stack_push(error_stack, ERROR_STATUS_RW_MEMORY_ALLOCATION_ERROR,
                     get_formatted_string(
                         "memory allocation error reading file: %s", filename));
    return NULL;
  }

  size_t bytes_read = fread(result_string, 1, file_size, file_handle);
  if (bytes_read != (size_t)file_size) {
    fclose_or_die(file_handle);
    free(result_string);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("error while reading file: %s", filename));
    return NULL;
  }

  result_string[file_size] = '\0';
  fclose_or_die(file_handle);

  return result_string;
}

FILE *fopen_or_die(const char *filename, const char *mode) {
  FILE *stream = fopen(filename, mode);
  if (!stream) {
    int error_number = errno;
    const char *system_error_message = strerror(error_number);
    log_fatal("failed to open file '%s' with mode %s: %s (%d)", filename, mode,
              system_error_message, error_number);
  }
  return stream;
}

FILE *fopen_safe(const char *filename, const char *mode,
                 ErrorStack *error_stack) {
  FILE *stream = fopen(filename, mode);
  if (!stream) {
    int error_number = errno;
    const char *system_error_message = strerror(error_number);
    error_stack_push(error_stack, ERROR_STATUS_RW_FAILED_TO_OPEN_STREAM,
                     get_formatted_string(
                         "failed to open file '%s' with mode %s: %s (%d)",
                         filename, mode, system_error_message, error_number));
  }
  return stream;
}

void fclose_or_die(FILE *stream) {
  if (fclose(stream) != 0) {
    int error_number = errno;
    const char *system_error_message = strerror(error_number);
    log_fatal("failed to close stream: %s (%d)", system_error_message,
              error_number);
  }
}

void fwrite_or_die(const void *ptr, size_t size, size_t nmemb, FILE *stream,
                   const char *description) {
  if (fwrite(ptr, size, nmemb, stream) != nmemb) {
    int error_number = errno;
    const char *system_error_message = strerror(error_number);
    log_fatal("%s fwrite failure: %s (%d)", description, system_error_message,
              error_number);
  }
}

FILE *popen_or_die(const char *command, const char *mode) {
  // Using popen is safe here because command is constructed with properly
  // quoted arguments
  FILE *pipe = popen(command, mode); // NOLINT(cert-env33-c)
  if (!pipe) {
    fprintf_or_die(stderr, "Failed to execute command: %s\n", command);
    exit(EXIT_FAILURE);
  }
  return pipe;
}