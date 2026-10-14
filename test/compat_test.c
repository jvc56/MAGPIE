#include "compat_test.h"

#include "../src/compat/cdir.h"
#include "../src/compat/cfile.h"
#include "../src/compat/cmap.h"
#include "../src/compat/cmemfile.h"
#include "../src/compat/cpath.h"
#include "../src/util/fileproxy.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
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

// Calls with side effects stay outside assert, which a release build drops.
static void remove_or_fail(const char *path) {
  const int result = remove(path);
  assert(result == 0);
}

static void puts_or_fail(const char *text, FILE *stream) {
  const int result = fputs(text, stream);
  assert(result >= 0);
}

static void assert_read(void *buffer, size_t capacity, FILE *stream,
                        size_t expected) {
  const size_t count = fread(buffer, 1, capacity, stream);
  assert(count == expected);
}

static void assert_next_byte(FILE *stream, int expected) {
  const int byte = fgetc(stream);
  assert(byte == expected);
}

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
  const int stream_error = ferror(stream);
  assert(!stream_error);
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
  assert_read(direct, sizeof(direct), stream, sizeof(fixture));
  assert(memcmp(direct, fixture, sizeof(fixture)) == 0);
  fclose_or_die(stream);
  remove_or_fail(compat_binary_path);
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

  remove_or_fail(compat_text_path);
  error_stack_destroy(error_stack);
}

// Sizes and offsets are 64-bit on every platform (ftell's long is 32 bits on
// Windows), and taking a stream's size leaves it where it was, for a file and
// for a precached buffer alike.
static void test_offsets(void) {
  char fixture[300];
  for (int byte_idx = 0; byte_idx < (int)sizeof(fixture); byte_idx++) {
    fixture[byte_idx] = (char)(byte_idx * 7);
  }
  write_fixture(compat_binary_path, fixture, sizeof(fixture));
  precache_file_data("compat_test_precached.tmp", fixture, sizeof(fixture));
  const char *paths[] = {compat_binary_path, "compat_test_precached.tmp"};
  for (int path_idx = 0; path_idx < 2; path_idx++) {
    ErrorStack *error_stack = error_stack_create();
    FILE *stream = stream_from_filename(paths[path_idx], error_stack);
    assert(error_stack_is_empty(error_stack));
    error_stack_destroy(error_stack);
    char head[10];
    assert_read(head, sizeof(head), stream, sizeof(head));
    int64_t size = 0;
    assert(cfile_stream_size(stream, &size));
    assert(size == (int64_t)sizeof(fixture));
    assert(cfile_tell(stream) == (int64_t)sizeof(head));
    assert(cfile_seek(stream, 250, SEEK_SET) == 0);
    assert(cfile_tell(stream) == 250);
    assert_next_byte(stream, (unsigned char)fixture[250]);
    assert(cfile_seek(stream, 299, SEEK_SET) == 0);
    assert_next_byte(stream, (unsigned char)fixture[299]);
    assert(cfile_seek(stream, 0, SEEK_END) == 0);
    assert(cfile_tell(stream) == (int64_t)sizeof(fixture));
    assert_next_byte(stream, EOF);
    fclose_or_die(stream);
  }
  fileproxy_destroy_cache();
  remove_or_fail(compat_binary_path);
}

static char *read_whole_file(const char *path) {
  ErrorStack *error_stack = error_stack_create();
  char *contents = get_string_from_file(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  return contents;
}

static void write_text(const char *path, const char *text) {
  write_fixture(path, text, strlen(text));
}

// A rename replaces an existing target in one step, as POSIX rename does;
// C's rename leaves that to the platform, and Windows' refuses.
static void test_rename_over_existing(void) {
  const char *from = "compat_test_from.tmp";
  write_text(compat_text_path, "old");
  write_text(from, "new");
  assert(cfile_rename(from, compat_text_path) == 0);
  assert(!cfile_access(from, CFILE_ACCESS_EXISTS));
  char *contents = read_whole_file(compat_text_path);
  assert(strings_equal(contents, "new"));
  free(contents);
  assert(cfile_rename(from, compat_text_path) != 0);
  remove_or_fail(compat_text_path);
}

// An atomic write replaces its target even while a reader has it open (as
// every data file is, through cfile_open), and the reader keeps reading what
// it opened. An abandoned write leaves the target alone.
static void test_atomic_write(void) {
  write_text(compat_text_path, "before");
  FILE *reader = cfile_open(compat_text_path, "rb", false);
  assert(reader);

  CFileAtomicWrite atomic_write;
  FILE *stream = cfile_atomic_write_begin(&atomic_write, compat_text_path);
  assert(stream);
  assert(!strings_equal(atomic_write.temporary, compat_text_path));
  assert(cfile_access(atomic_write.temporary, CFILE_ACCESS_EXISTS));
  puts_or_fail("after", stream);
  assert(cfile_atomic_write_end(&atomic_write, true));

  char held[16] = {0};
  assert_read(held, sizeof(held) - 1, reader, strlen("before"));
  assert(strings_equal(held, "before"));
  fclose_or_die(reader);
  char *contents = read_whole_file(compat_text_path);
  assert(strings_equal(contents, "after"));
  free(contents);

  stream = cfile_atomic_write_begin(&atomic_write, compat_text_path);
  assert(stream);
  char *temporary = string_duplicate(atomic_write.temporary);
  puts_or_fail("abandoned", stream);
  assert(cfile_atomic_write_end(&atomic_write, false));
  assert(!cfile_access(temporary, CFILE_ACCESS_EXISTS));
  free(temporary);
  contents = read_whole_file(compat_text_path);
  assert(strings_equal(contents, "after"));
  free(contents);

  // A target that does not exist yet is created.
  remove_or_fail(compat_text_path);
  stream = cfile_atomic_write_begin(&atomic_write, compat_text_path);
  assert(stream);
  puts_or_fail("created", stream);
  assert(cfile_atomic_write_end(&atomic_write, true));
  contents = read_whole_file(compat_text_path);
  assert(strings_equal(contents, "created"));
  free(contents);
  remove_or_fail(compat_text_path);
}

static void test_directories_and_access(void) {
  const char *directory = "compat_test_directory.tmp";
  assert(!cfile_access(directory, CFILE_ACCESS_EXISTS));
  assert(cfile_mkdir(directory) == 0);
  assert(cfile_mkdir(directory) != 0);
  assert(cfile_is_directory(directory));
  assert(cfile_access(directory, CFILE_ACCESS_EXISTS));
  assert(cfile_access(directory, CFILE_ACCESS_WRITE));

  char *file = get_formatted_string("%s/file.tmp", directory);
  write_text(file, "x");
  assert(!cfile_is_directory(file));
  assert(cfile_access(file, CFILE_ACCESS_READ | CFILE_ACCESS_WRITE));
  // A directory with a file in it is not removed.
  assert(cfile_rmdir(directory) != 0);
  remove_or_fail(file);
  free(file);
  assert(cfile_rmdir(directory) == 0);
  assert(!cfile_is_directory(directory));
  assert(!cfile_is_directory(NULL));
}

// Every mode MAGPIE passes is accepted everywhere, glibc's "e" included,
// which UCRT's fopen would abort on.
static void test_open_modes(void) {
  FILE *stream = cfile_open(compat_text_path, "we", false);
  assert(stream);
  puts_or_fail("one", stream);
  fclose_or_die(stream);
  stream = cfile_open(compat_text_path, "ab", false);
  assert(stream);
  puts_or_fail("two", stream);
  fclose_or_die(stream);
  stream = cfile_open(compat_text_path, "r+b", false);
  assert(stream);
  char text[8] = {0};
  assert_read(text, 6, stream, 6);
  assert(strings_equal(text, "onetwo"));
  fclose_or_die(stream);
  stream = cfile_open(compat_text_path, "w+", false);
  assert(stream);
  puts_or_fail("three", stream);
  assert(cfile_seek(stream, 0, SEEK_SET) == 0);
  memset(text, 0, sizeof(text));
  assert_read(text, sizeof(text) - 1, stream, 5);
  assert(strings_equal(text, "three"));
  fclose_or_die(stream);
  assert(!cfile_open(compat_text_path, "wb", true));
  assert(!cfile_open(compat_text_path, "q", false));
  assert(!cfile_open(compat_text_path, "rb", true));
  remove_or_fail(compat_text_path);
  assert(!cfile_open(compat_text_path, "rb", false));
}

// Lines come back whole, however long, with or without a final newline.
static void test_getline(void) {
  StringBuilder *builder = string_builder_create();
  for (int char_idx = 0; char_idx < 1000; char_idx++) {
    string_builder_add_char(builder, (char)('a' + char_idx % 26));
  }
  string_builder_add_string(builder, "\nshort\r\n\nlast");
  write_fixture(compat_text_path, string_builder_peek(builder),
                string_builder_length(builder));
  FILE *stream = cfile_open(compat_text_path, "rb", false);
  assert(stream);
  char *line = NULL;
  size_t capacity = 0;
  assert(cfile_getline(&line, &capacity, stream) == 1001);
  assert(line[999] == 'a' + 999 % 26 && line[1000] == '\n');
  assert(getline_ignore_carriage_return(&line, &capacity, stream) == 6);
  assert(strings_equal(line, "short\n"));
  assert(cfile_getline(&line, &capacity, stream) == 1);
  assert(strings_equal(line, "\n"));
  assert(cfile_getline(&line, &capacity, stream) == 4);
  assert(strings_equal(line, "last"));
  assert(cfile_getline(&line, &capacity, stream) == -1);
  free(line);
  fclose_or_die(stream);
  string_builder_destroy(builder);
  remove_or_fail(compat_text_path);
}

static void assert_path_list(const char *list, int expected_count,
                             const char *const *expected) {
  char **paths = NULL;
  const int count = cpath_split_list(list, &paths);
  assert(count == expected_count);
  for (int path_idx = 0; path_idx < count; path_idx++) {
    assert(strings_equal(paths[path_idx], expected[path_idx]));
  }
  cpath_list_destroy(paths, count);
}

// Lists of paths split on ':' everywhere, and on Windows also on ';' and
// never on a drive letter's colon.
static void test_path_lists(void) {
  assert_path_list("data", 1, (const char *const[]){"data"});
  assert_path_list("data:more", 2, (const char *const[]){"data", "more"});
  assert_path_list("::data::more:", 2, (const char *const[]){"data", "more"});
  assert_path_list("", 0, NULL);
  char *first = cpath_list_first("first:second");
  assert(strings_equal(first, "first"));
  free(first);
  first = cpath_list_first("");
  assert(strings_equal(first, ""));
  free(first);
  assert(strings_equal(cpath_basename("dir/sub/name.gcg"), "name.gcg"));
  assert(strings_equal(cpath_basename("name.gcg"), "name.gcg"));
  assert(cpath_is_separator('/'));
  // Backslash separates directories on Windows only, which is how this test
  // tells which platform's rules to expect without testing platform macros.
  if (cpath_is_separator('\\')) {
    assert_path_list("C:\\magpie\\data", 1,
                     (const char *const[]){"C:\\magpie\\data"});
    assert_path_list(
        "C:\\magpie\\data;D:/more:data", 3,
        (const char *const[]){"C:\\magpie\\data", "D:/more", "data"});
    assert_path_list("x;y", 2, (const char *const[]){"x", "y"});
    // One letter and a colon is a drive.
    assert_path_list("d:data", 1, (const char *const[]){"d:data"});
    assert(strings_equal(cpath_basename("dir\\name.gcg"), "name.gcg"));
  } else {
    assert_path_list("x;y", 1, (const char *const[]){"x;y"});
    assert_path_list("C:/data", 2, (const char *const[]){"C", "/data"});
  }
  char *executable_dir = cpath_executable_dir();
  assert(executable_dir && executable_dir[0]);
  assert(cfile_is_directory(executable_dir));
  free(executable_dir);
}

// A directory lists each entry once, without "." and "..".
static void test_directory_listing(void) {
  const char *directory = "compat_test_listing.tmp";
  assert(cfile_mkdir(directory) == 0);
  const char *names[] = {"b.txt", "a.kwg", ".hidden"};
  for (int name_idx = 0; name_idx < 3; name_idx++) {
    char *path = get_formatted_string("%s/%s", directory, names[name_idx]);
    write_text(path, names[name_idx]);
    free(path);
  }
  CDir *dir = cdir_open(directory);
  assert(dir);
  bool seen[3] = {false, false, false};
  int entries = 0;
  const char *entry;
  while ((entry = cdir_next(dir)) != NULL) {
    entries++;
    for (int name_idx = 0; name_idx < 3; name_idx++) {
      if (strings_equal(entry, names[name_idx])) {
        assert(!seen[name_idx]);
        seen[name_idx] = true;
      }
    }
  }
  cdir_close(dir);
  assert(entries == 3 && seen[0] && seen[1] && seen[2]);

  int num_files = 0;
  ErrorStack *error_stack = error_stack_create();
  char **files =
      get_files_in_directory(directory, ".txt", &num_files, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(num_files == 1 && strings_equal(files[0], "b.txt"));
  free(files[0]);
  free(files);
  error_stack_destroy(error_stack);

  for (int name_idx = 0; name_idx < 3; name_idx++) {
    char *path = get_formatted_string("%s/%s", directory, names[name_idx]);
    remove_or_fail(path);
    free(path);
  }
  assert(cfile_rmdir(directory) == 0);
  assert(!cdir_open(directory));
}

// A mapped file reads as the bytes in it, and a missing or empty file does
// not map. Bytes precached as a stream read back the same way.
static void test_mapping(void) {
  char fixture[5000];
  for (int byte_idx = 0; byte_idx < (int)sizeof(fixture); byte_idx++) {
    fixture[byte_idx] = (char)(byte_idx * 13);
  }
  write_fixture(compat_binary_path, fixture, sizeof(fixture));
  CMap map;
  assert(cmap_open(&map, compat_binary_path, true));
  assert(map.size == sizeof(fixture));
  assert(memcmp(map.data, fixture, sizeof(fixture)) == 0);
  cmap_close(&map);
  assert(!map.data);
  cmap_close(&map);
  write_fixture(compat_binary_path, fixture, 0);
  assert(!cmap_open(&map, compat_binary_path, false));
  remove_or_fail(compat_binary_path);
  assert(!cmap_open(&map, compat_binary_path, false));

  FILE *stream = cmemfile_open(fixture, sizeof(fixture));
  assert(stream);
  char read_back[sizeof(fixture) + 1];
  assert_read(read_back, sizeof(read_back), stream, sizeof(fixture));
  assert(memcmp(read_back, fixture, sizeof(fixture)) == 0);
  fclose_or_die(stream);
}

void test_compat(void) {
  test_binary_round_trip();
  test_text_line_endings();
  test_offsets();
  test_rename_over_existing();
  test_atomic_write();
  test_directories_and_access();
  test_open_modes();
  test_getline();
  test_path_lists();
  test_directory_listing();
  test_mapping();
}
