#include "compat_test.h"

#include "../src/compat/cfile.h"
#include "../src/util/fileproxy.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Contract tests for the platform layer: each one states behavior that must
// hold on every OS MAGPIE builds for, and runs unchanged on Linux, macOS and
// Windows (natively and under Wine). See docs/windows-plan.md, section 5.

enum {
  // Every byte value, then the two that text mode on Windows mangles.
  COMPAT_BINARY_FIXTURE_SIZE = 256 + 6,
};

static const char compat_binary_path[] = "compat_test_binary.tmp";
static const char compat_text_path[] = "compat_test_text.tmp";

static void write_fixture(const char *path, const char *bytes, size_t size) {
  ErrorStack *error_stack = error_stack_create();
  FILE *stream = fopen_safe(path, "wb", error_stack);
  assert(error_stack_is_empty(error_stack));
  fwrite_or_die(bytes, 1, size, stream, "compat fixture");
  fclose_or_die(stream);
  error_stack_destroy(error_stack);
}

// Reads every byte of path through the reader every data file uses.
static char *read_fixture(const char *path, size_t *size) {
  ErrorStack *error_stack = error_stack_create();
  FILE *stream = stream_from_filename(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  size_t capacity = 1024;
  char *bytes = malloc_or_die(capacity);
  *size = 0;
  size_t bytes_read;
  while ((bytes_read = fread(bytes + *size, 1, capacity - *size, stream)) > 0) {
    *size += bytes_read;
    if (*size == capacity) {
      capacity *= 2;
      bytes = realloc_or_die(bytes, capacity);
    }
  }
  assert(!ferror(stream));
  fclose_or_die(stream);
  return bytes;
}

// Binary data read back is byte for byte what was written: no \r\n
// translation and no end of file at 0x1A, as Windows text mode would do.
static void test_binary_round_trip(void) {
  char fixture[COMPAT_BINARY_FIXTURE_SIZE];
  for (int byte_idx = 0; byte_idx < 256; byte_idx++) {
    fixture[byte_idx] = (char)byte_idx;
  }
  memcpy(fixture + 256, "\r\n\x1a\n\r\n", 6);
  write_fixture(compat_binary_path, fixture, sizeof(fixture));

  size_t size = 0;
  char *bytes = read_fixture(compat_binary_path, &size);
  assert(size == sizeof(fixture));
  assert(memcmp(bytes, fixture, sizeof(fixture)) == 0);
  free(bytes);

  FILE *stream = cfile_open(compat_binary_path, "rb", false);
  assert(stream);
  char direct[COMPAT_BINARY_FIXTURE_SIZE + 1];
  assert(fread(direct, 1, sizeof(direct), stream) == sizeof(fixture));
  assert(memcmp(direct, fixture, sizeof(fixture)) == 0);
  fclose_or_die(stream);
  assert(remove(compat_binary_path) == 0);
}

// Text files MAGPIE writes have \n line endings on every platform, and text
// read back has \r\n turned into \n, so a file written on Windows (or by a
// Windows editor) reads the same everywhere.
static void test_text_line_endings(void) {
  ErrorStack *error_stack = error_stack_create();
  write_string_to_file(compat_text_path, "wb", "one\ntwo\n", error_stack);
  assert(error_stack_is_empty(error_stack));
  size_t size = 0;
  char *bytes = read_fixture(compat_text_path, &size);
  assert(size == strlen("one\ntwo\n"));
  assert(memcmp(bytes, "one\ntwo\n", size) == 0);
  free(bytes);

  // A lone \r is kept, and 0x1A does not end the text.
  const char crlf[] = "one\r\ntwo\r\n\x1a three\rfour\r\n";
  write_fixture(compat_text_path, crlf, sizeof(crlf) - 1);
  char *text = get_string_from_file(compat_text_path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(strings_equal(text, "one\ntwo\n\x1a three\rfour\n"));
  free(text);

  text = fileproxy_get_string_from_filename(compat_text_path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(strings_equal(text, "one\ntwo\n\x1a three\rfour\n"));
  free(text);

  // Larger than any read buffer, so the text is read in pieces.
  StringBuilder *builder = string_builder_create();
  for (int line_idx = 0; line_idx < 5000; line_idx++) {
    string_builder_add_formatted_string(builder, "line %d\r\n", line_idx);
  }
  write_fixture(compat_text_path, string_builder_peek(builder),
                string_builder_length(builder));
  text = get_string_from_file(compat_text_path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(strlen(text) == string_builder_length(builder) - 5000);
  assert(strncmp(text, "line 0\nline 1\n", strlen("line 0\nline 1\n")) == 0);
  free(text);
  string_builder_destroy(builder);

  assert(remove(compat_text_path) == 0);
  error_stack_destroy(error_stack);
}

void test_compat(void) {
  test_binary_round_trip();
  test_text_line_endings();
}
