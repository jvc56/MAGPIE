#include "io_util.h"

#include "../compat/cdir.h"
#include "../compat/cfile.h"
#include "../compat/cpath.h"
#include "../compat/cpthread_defs.h"
#include "../compat/cterm.h"
#include "../compat/ctime.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ERROR_STACK_CAPACITY = 100 };

static log_level_t current_log_level = LOG_FATAL;
static FILE *stream_out = NULL;
static FILE *stream_err = NULL;
static FILE *stream_in = NULL;
// Where io_wait_before_close reads its line, when a reader of the input has
// been set (io_set_line_reader).
static char *(*line_reader)(void) = NULL;
static cpthread_mutex_t stream_mutex = CPTHREAD_MUTEX_INITIALIZER;

// Local mutex wrappers to avoid circular dependency with cpthread.h
static inline void stream_mutex_lock(void) {
  if (cpthread_mutex_lock_status(&stream_mutex)) {
    (void)fprintf(stderr, "FATAL: stream mutex lock failed\n");
    abort();
  }
}

static inline void stream_mutex_unlock(void) {
  if (cpthread_mutex_unlock_status(&stream_mutex)) {
    (void)fprintf(stderr, "FATAL: stream mutex unlock failed\n");
    abort();
  }
}

FILE *get_stream_out(void) {
  stream_mutex_lock();
  FILE *stream = stream_out;
  if (!stream) {
    stream = stdout;
  }
  stream_mutex_unlock();
  return stream;
}

FILE *get_stream_err(void) {
  stream_mutex_lock();
  FILE *stream = stream_err;
  if (!stream) {
    stream = stderr;
  }
  stream_mutex_unlock();
  return stream;
}

FILE *get_stream_in(void) {
  stream_mutex_lock();
  FILE *stream = stream_in;
  if (!stream) {
    stream = stdin;
  }
  stream_mutex_unlock();
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

void snprintf_or_die(char *buf, size_t buf_size, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int result = vsnprintf(buf, buf_size, format, args);
  va_end(args);
  if (result < 0 || (size_t)result >= buf_size) {
    log_fatal("snprintf failed for format: %s with buffer size: %zu", format,
              buf_size);
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

  char time_buf[64] = "";
  struct tm local;
  if (cfile_localtime(time(NULL), &local)) {
    time_buf[strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                      &local)] = '\0';
  }
  fprintf_or_die(output_fh, "[%s] %-5s %s:%d: ", time_buf, level_string,
                 caller_filename, caller_line);
  va_list args;
  va_start(args, format);
  write_to_stream_with_vargs(output_fh, false, format, args);
  va_end(args);
  fprintf_or_die(output_fh, "\n");
  fflush_or_die(output_fh);

  if (exit_fatally) {
    io_wait_before_close();
    cterm_restore();
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

int64_t getline_ignore_carriage_return(char **lineptr, size_t *n,
                                       FILE *stream) {
  int64_t nread = cfile_getline(lineptr, n, stream);
  if (nread <= 0) {
    return nread;
  }
  if (nread >= 2 && (*lineptr)[nread - 2] == '\r' &&
      (*lineptr)[nread - 1] == '\n') {
    memmove(&(*lineptr)[nread - 2], &(*lineptr)[nread - 1], 2);
    nread--;
  } else if ((*lineptr)[nread - 1] == '\r') {
    (*lineptr)[nread - 1] = '\0';
    nread--;
  }
  return nread;
}

char *read_line_from_stream(FILE *stream) {
  char *line = NULL;
  size_t len = 0;
  int64_t read;
  errno = 0;
  read = getline_ignore_carriage_return(&line, &len, stream);
  if (read == -1) {
    int error_number = errno;
    if (error_number) {
      perror("getline");
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

char *read_line_from_stream_in(void) {
  return read_line_from_stream(get_stream_in());
}

void io_set_line_reader(char *(*reader)(void)) { line_reader = reader; }

void io_wait_before_close(void) {
  if (!cterm_owns_console()) {
    return;
  }
  write_to_stream_err("\nPress Enter to close this window.");
  char *line = line_reader ? line_reader() : read_line_from_stream_in();
  free(line);
}

// WARNING: This function should only be called once at startup or for testing
void log_set_level(log_level_t new_log_level) {
  current_log_level = new_log_level;
}

// WARNING: This function should only be used for testing
void io_set_stream_out(FILE *stream) {
  stream_mutex_lock();
  stream_out = stream;
  stream_mutex_unlock();
}

// WARNING: This function should only be used for testing
void io_reset_stream_out(void) {
  stream_mutex_lock();
  stream_out = NULL;
  stream_mutex_unlock();
}

// WARNING: This function should only be used for testing
void io_set_stream_err(FILE *stream) {
  stream_mutex_lock();
  stream_err = stream;
  stream_mutex_unlock();
}

// WARNING: This function should only be used for testing
void io_reset_stream_err(void) {
  stream_mutex_lock();
  stream_err = NULL;
  stream_mutex_unlock();
}

// WARNING: This function should only be used for testing
void io_set_stream_in(FILE *stream) {
  stream_mutex_lock();
  stream_in = stream;
  stream_mutex_unlock();
}

// WARNING: This function should only be used for testing
void io_reset_stream_in(void) {
  stream_mutex_lock();
  stream_in = NULL;
  stream_mutex_unlock();
}

void *malloc_or_die(size_t size) {
  void *uncasted_pointer = malloc(size);
  if (!uncasted_pointer) {
    log_fatal("failed to malloc size of %zu", size);
  }
  return uncasted_pointer;
}

void *calloc_or_die(size_t number_of_elements, size_t size_of_element) {
  void *uncasted_pointer = calloc(number_of_elements, size_of_element);
  if (!uncasted_pointer) {
    log_fatal("failed to calloc %zu elements of size %zu", number_of_elements,
              size_of_element);
  }
  return uncasted_pointer;
}

void *realloc_or_die(void *realloc_target, size_t size) {
  void *realloc_result = realloc(realloc_target, size);
  if (!realloc_result) {
    log_fatal("failed to realloc %p with size of %zu", realloc_target, size);
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
  write_to_stream_err("%s", error_string);
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

void fseek_or_die(FILE *stream, int64_t offset, int whence) {
  if (cfile_seek(stream, offset, whence) != 0) {
    int error_number = errno;
    const char *system_error_message = strerror(error_number);
    log_fatal("failed to fseek stream: %s (%d)", system_error_message,
              error_number);
  }
}

char *get_string_from_file_handle(FILE *file_handle, const char *filename,
                                  ErrorStack *error_stack) {
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }

  // Read to the end rather than trusting the file's size, which a text-mode
  // stream on Windows reads short of and a precached stream does not have.
  size_t capacity = 4096;
  size_t length = 0;
  char *contents = (char *)malloc_or_die(capacity);
  while (true) {
    if (capacity - length < 2) {
      capacity *= 2;
      contents = (char *)realloc_or_die(contents, capacity);
    }
    // Leave room for the null terminator.
    const size_t bytes_read =
        fread(contents + length, 1, capacity - length - 1, file_handle);
    length += bytes_read;
    if (bytes_read == 0) {
      break;
    }
  }

  if (ferror(file_handle)) {
    fclose_or_die(file_handle);
    free(contents);
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("error while reading file: %s", filename));
    return NULL;
  }
  fclose_or_die(file_handle);

  // Text files written on Windows end their lines with \r\n, and such files
  // reach every platform (a GCG saved by a Windows editor, a downloaded CSV),
  // so on every platform each \r\n becomes \n: every caller then sees the
  // same text wherever the file came from. Only the pair is rewritten; a lone
  // \r is left as it is.
  size_t kept = 0;
  for (size_t byte_idx = 0; byte_idx < length; byte_idx++) {
    if (contents[byte_idx] == '\r' && byte_idx + 1 < length &&
        contents[byte_idx + 1] == '\n') {
      continue;
    }
    contents[kept++] = contents[byte_idx];
  }
  contents[kept] = '\0';
  return contents;
}

char *get_string_from_file(const char *filename, ErrorStack *error_stack) {
  FILE *file_handle = fopen_safe(filename, "rb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  return get_string_from_file_handle(file_handle, filename, error_stack);
}

FILE *fopen_or_die(const char *filename, const char *mode) {
  FILE *stream = cfile_open(filename, mode, false);
  if (!stream) {
    int error_number = errno;
    const char *system_error_message = strerror(error_number);
    log_fatal("failed to open file '%s' with mode %s: %s (%d)", filename, mode,
              system_error_message, error_number);
  }
  return stream;
}

FILE *fopen_exclusive_safe(const char *filename, const char *mode,
                           ErrorStack *error_stack) {
  FILE *stream = cfile_open(filename, mode, true);
  if (!stream) {
    const int error_number = errno;
    error_stack_push(error_stack, ERROR_STATUS_RW_FAILED_TO_OPEN_STREAM,
                     get_formatted_string("failed to create file '%s': %s (%d)",
                                          filename, strerror(error_number),
                                          error_number));
  }
  return stream;
}

FILE *fopen_safe(const char *filename, const char *mode,
                 ErrorStack *error_stack) {
  FILE *stream = cfile_open(filename, mode, false);
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

void append_string_to_file(const char *filename, const char *string,
                           ErrorStack *error_stack) {
  write_string_to_file(filename, "ab", string, error_stack);
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

bool path_is_directory(const char *path) {
  if (!path) {
    return false;
  }
  return cfile_is_directory(path);
}

// (size, mtime) alone collides: a file replaced with different bytes of the
// same size inside one mtime tick is identical to the old one, and archive
// extraction routinely sets mtimes rather than letting them fall to now. The
// inode and the ctime close that -- ctime moves on any change to the inode and
// cannot be set backwards by a program -- but only at the resolution they are
// read at: whole seconds is not enough, because the whole problem is two
// writes inside one tick. So the identity carries nanoseconds where the
// filesystem records them.
char *get_file_identity(const char *path) {
  CFileIdentity identity;
  if (!cfile_identity(path, &identity)) {
    return NULL;
  }
  return get_formatted_string(
      "%lld|%lld.%09ld|%llu|%lld.%09ld", (long long)identity.size,
      (long long)identity.mtime_seconds, identity.mtime_nanoseconds,
      (unsigned long long)identity.inode, (long long)identity.ctime_seconds,
      identity.ctime_nanoseconds);
}

enum {
  // A writer touches its temporary file continuously, and the largest (a
  // rack info table) takes minutes; one untouched this long was abandoned.
  STALE_TEMPORARY_SECONDS = 60 * 60,
  // The XXXXXX mkstemp fills in.
  MKSTEMP_SUFFIX_LENGTH = 6,
};

// Whether `text` is one or more digits and dashes, or, with
// `mkstemp_suffix`, also mkstemp's six letters and digits.
static bool is_temporary_tag(const char *text, bool mkstemp_suffix) {
  bool digits_and_dashes = text[0] != '\0';
  bool alphanumeric = text[0] != '\0';
  size_t length = 0;
  for (; text[length]; length++) {
    const unsigned char c = (unsigned char)text[length];
    digits_and_dashes = digits_and_dashes && (isdigit(c) || c == '-');
    alphanumeric = alphanumeric && isalnum(c);
  }
  return digits_and_dashes ||
         (mkstemp_suffix && alphanumeric && length == MKSTEMP_SUFFIX_LENGTH);
}

// Whether `entry` is what an unfinished write of `base` leaves beside it:
// `<base>.<pid>-<n>-<ns>.tmp` (or `<base>.<pid>.tmp`, the earlier form) from
// temporary_sibling, `<base>.tmp.<tag>` from cfile_atomic_write_begin, or
// `<base>.old.<pid>-<n>`, a replaced file cfile_rename could not delete on
// Windows.
static bool is_temporary_sibling_of(const char *entry, const char *base) {
  const size_t base_length = strlen(base);
  if (strncmp(entry, base, base_length) != 0 || entry[base_length] != '.') {
    return false;
  }
  const char *rest = entry + base_length + 1;
  if (strncmp(rest, "tmp.", 4) == 0) {
    return is_temporary_tag(rest + 4, true);
  }
  if (strncmp(rest, "old.", 4) == 0) {
    return is_temporary_tag(rest + 4, false);
  }
  const char *end = rest;
  while (isdigit((unsigned char)*end) || *end == '-') {
    end++;
  }
  return end > rest && strcmp(end, ".tmp") == 0;
}

void remove_stale_temporary_siblings(const char *filename) {
  const char *base = cpath_basename(filename);
  char *dir =
      base != filename
          ? get_formatted_string("%.*s", (int)(base - filename - 1), filename)
          : get_formatted_string("%s", ".");
  CDir *listing = cdir_open(dir[0] ? dir : "/");
  if (!listing) {
    free(dir);
    return;
  }
  const time_t now = time(NULL);
  const char *entry;
  while ((entry = cdir_next(listing)) != NULL) {
    if (!is_temporary_sibling_of(entry, base)) {
      continue;
    }
    char *path = get_formatted_string("%s/%s", dir, entry);
    // A replaced file moved aside keeps its old modification time, so for
    // one the rename (a status change) has to be an hour old too.
    const bool moved_aside =
        strncmp(entry + strlen(base), ".old.", strlen(".old.")) == 0;
    CFileIdentity identity;
    if (cfile_identity(path, &identity) &&
        now - identity.mtime_seconds > STALE_TEMPORARY_SECONDS &&
        (!moved_aside ||
         now - identity.ctime_seconds > STALE_TEMPORARY_SECONDS)) {
      (void)remove(path);
    }
    free(path);
  }
  cdir_close(listing);
  free(dir);
}

char *temporary_sibling(const char *filename) {
  remove_stale_temporary_siblings(filename);
  // The process id, a per-process counter and the clock to the nanosecond.
  // The first two alone are unique within one PID space, but two containers
  // sharing a data volume can both be PID 1 with their counters in step (the
  // same generation's KLV, fetched at the same point in two runs). Writers
  // still open the name exclusively (fopen_exclusive_safe), so a clash fails
  // rather than truncating the other's file.
  static atomic_uint_least64_t counter = 0;
  const unsigned long long sequence =
      (unsigned long long)atomic_fetch_add(&counter, 1);
  TimeSpec now = {0};
  ctimer_clock_gettime_realtime(&now);
  const unsigned long long nonce =
      (unsigned long long)now.tv_sec * 1000000000ULL +
      (unsigned long long)now.tv_nsec;
  return get_formatted_string("%s.%ld-%llu-%llu.tmp", filename,
                              cfile_process_id(), sequence, nonce);
}

void rename_into_place(const char *temporary, const char *filename,
                       const char *what, ErrorStack *error_stack) {
  if (cfile_rename(temporary, filename) != 0) {
    const int error_number = errno;
    (void)remove(temporary);
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     get_formatted_string("could not write %s %s: %s", what,
                                          filename, strerror(error_number)));
  }
}

enum {
  FILENAME_SORT_DIGIT_BUFFER_SIZE = 256,
};

// Strips every non-digit character from filename and parses the remaining
// digits as a single base-10 number (0 if there are none). Digits beyond
// FILENAME_SORT_DIGIT_BUFFER_SIZE - 1 are dropped.
static unsigned long long extract_digits_as_number(const char *filename) {
  char digits[FILENAME_SORT_DIGIT_BUFFER_SIZE];
  size_t digit_count = 0;
  for (const char *ch = filename;
       *ch != '\0' && digit_count < sizeof(digits) - 1; ch++) {
    if (isdigit((unsigned char)*ch)) {
      digits[digit_count++] = *ch;
    }
  }
  digits[digit_count] = '\0';
  if (digit_count == 0) {
    return 0;
  }
  return strtoull(digits, NULL, 10);
}

// Sorts ascending by the number formed from each filename's digit
// characters (non-digits removed); ties fall back to alphabetical order.
static int compare_filenames_for_sort(const void *a, const void *b) {
  const char *name_a = *(const char *const *)a;
  const char *name_b = *(const char *const *)b;
  const unsigned long long num_a = extract_digits_as_number(name_a);
  const unsigned long long num_b = extract_digits_as_number(name_b);
  if (num_a != num_b) {
    return (num_a < num_b) ? -1 : 1;
  }
  return strcmp(name_a, name_b);
}

char **get_files_in_directory(const char *dir_path, const char *suffix,
                              int *num_files, ErrorStack *error_stack) {
  CDir *dir = cdir_open(dir_path);
  if (!dir) {
    const int error_number = errno;
    error_stack_push(
        error_stack, ERROR_STATUS_RW_FAILED_TO_OPEN_STREAM,
        get_formatted_string("failed to open directory '%s': %s (%d)", dir_path,
                             strerror(error_number), error_number));
    *num_files = 0;
    return NULL;
  }
  int capacity = 16;
  char **files = malloc_or_die(sizeof(char *) * (size_t)capacity);
  int count = 0;
  const size_t suffix_len = strlen(suffix);
  const char *entry;
  while ((entry = cdir_next(dir)) != NULL) {
    const size_t name_len = strlen(entry);
    if (name_len < suffix_len ||
        strcmp(entry + name_len - suffix_len, suffix) != 0) {
      continue;
    }
    if (count == capacity) {
      capacity *= 2;
      files = realloc_or_die(files, sizeof(char *) * (size_t)capacity);
    }
    const size_t copy_size = name_len + 1;
    char *name_copy = malloc_or_die(copy_size);
    memcpy(name_copy, entry, copy_size);
    files[count++] = name_copy;
  }
  cdir_close(dir);
  qsort(files, (size_t)count, sizeof(char *), compare_filenames_for_sort);
  *num_files = count;
  return files;
}