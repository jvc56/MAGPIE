#include "data_fetch_test.h"

#include "../src/compat/cfile.h"
#include "../src/compat/cpath.h"
#include "../src/impl/data_fetch.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_platform.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Unpacking data tarballs as other tools write them: data_fetch's own
// download is pinned to one tarball, so these build theirs here, a tar stream
// in a gzip member of stored (uncompressed) deflate blocks.

enum {
  TAR_BLOCK = 512,
  MAX_TARBALL_BYTES = 16 * TAR_BLOCK,
  STORED_BLOCK_MAX = 65535,
};

typedef struct Tarball {
  unsigned char bytes[MAX_TARBALL_BYTES];
  size_t length;
} Tarball;

static void tar_octal(unsigned char *field, size_t width, uint64_t value) {
  (void)snprintf((char *)field, width, "%0*llo", (int)width - 1,
                 (unsigned long long)value);
}

// Appends a ustar header for `name` of `type` and `size` bytes, linking to
// `link` (or not, for NULL), then `contents`, padded to a whole block.
static void tar_add(Tarball *tar, const char *name, char type, const char *link,
                    const char *contents) {
  const size_t size = contents ? strlen(contents) : 0;
  const size_t padded = (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
  assert(tar->length + TAR_BLOCK + padded <= MAX_TARBALL_BYTES);
  unsigned char *header = tar->bytes + tar->length;
  memset(header, 0, TAR_BLOCK);
  memcpy(header, name, strlen(name));
  tar_octal(header + 100, 8, type == '5' ? 0755 : 0644);
  tar_octal(header + 108, 8, 0);
  tar_octal(header + 116, 8, 0);
  tar_octal(header + 124, 12, size);
  tar_octal(header + 136, 12, 0);
  header[156] = (unsigned char)type;
  if (link) {
    memcpy(header + 157, link, strlen(link));
  }
  memcpy(header + 257, "ustar", 6);
  memcpy(header + 263, "00", 2);
  memset(header + 148, ' ', 8);
  unsigned int checksum = 0;
  for (int byte_idx = 0; byte_idx < TAR_BLOCK; byte_idx++) {
    checksum += header[byte_idx];
  }
  tar_octal(header + 148, 7, checksum);
  tar->length += TAR_BLOCK;
  memset(tar->bytes + tar->length, 0, padded);
  if (size > 0) {
    memcpy(tar->bytes + tar->length, contents, size);
  }
  tar->length += padded;
}

static void tar_end(Tarball *tar) {
  assert(tar->length + 2 * TAR_BLOCK <= MAX_TARBALL_BYTES);
  memset(tar->bytes + tar->length, 0, 2 * TAR_BLOCK);
  tar->length += 2 * TAR_BLOCK;
}

static uint32_t crc32_of(const unsigned char *bytes, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t byte_idx = 0; byte_idx < length; byte_idx++) {
    crc ^= bytes[byte_idx];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return crc ^ 0xffffffffU;
}

static void put_le(FILE *stream, uint32_t value, int bytes) {
  for (int byte_idx = 0; byte_idx < bytes; byte_idx++) {
    const unsigned char byte = (unsigned char)(value >> (8 * byte_idx));
    fwrite_or_die(&byte, 1, 1, stream, "test gzip");
  }
}

// Writes `tar` to `path` as a .tgz.
static void write_tgz(const Tarball *tar, const char *path) {
  ErrorStack *error_stack = error_stack_create();
  FILE *stream = fopen_safe(path, "wb", error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  static const unsigned char gzip_header[] = {0x1f, 0x8b, 8, 0, 0,
                                              0,    0,    0, 0, 0xff};
  fwrite_or_die(gzip_header, 1, sizeof(gzip_header), stream, "test gzip");
  size_t written = 0;
  do {
    size_t count = tar->length - written;
    if (count > STORED_BLOCK_MAX) {
      count = STORED_BLOCK_MAX;
    }
    const bool last = written + count == tar->length;
    put_le(stream, last ? 1U : 0U, 1);
    put_le(stream, (uint32_t)count, 2);
    put_le(stream, (uint32_t)(~count & 0xffffU), 2);
    fwrite_or_die(tar->bytes + written, 1, count, stream, "test gzip");
    written += count;
  } while (written < tar->length);
  put_le(stream, crc32_of(tar->bytes, tar->length), 4);
  put_le(stream, (uint32_t)tar->length, 4);
  fclose_or_die(stream);
}

// Unpacks `tar` into `data_dir`; returns the error message, or NULL.
static char *unpack(const Tarball *tar, const char *archive_path,
                    const char *data_dir) {
  write_tgz(tar, archive_path);
  ErrorStack *error_stack = error_stack_create();
  data_fetch_unpack_archive(archive_path, data_dir, error_stack);
  char *message = error_stack_is_empty(error_stack)
                      ? NULL
                      : error_stack_get_string_and_reset(error_stack);
  error_stack_destroy(error_stack);
  return message;
}

static void assert_file_holds(const char *directory, const char *name,
                              const char *expected) {
  char *path = get_formatted_string("%s/%s", directory, name);
  ErrorStack *error_stack = error_stack_create();
  char *contents = get_string_from_file(path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(strings_equal(contents, expected));
  free(contents);
  error_stack_destroy(error_stack);
  free(path);
}

static bool path_exists(const char *directory, const char *name) {
  char *path = get_formatted_string("%s/%s", directory, name);
  const bool exists = cfile_access(path, CFILE_ACCESS_EXISTS);
  free(path);
  return exists;
}

// The headers git archive and `tar --format=posix` add are skipped, whatever
// they are called, and the data around them unpacks, links as copies.
static void test_pax_headers_are_skipped(const char *root) {
  Tarball tar = {.length = 0};
  tar_add(&tar, "pax_global_header", 'g', NULL,
          "52 comment=0123456789abcdef0123456789abcdef01234567\n");
  tar_add(&tar, "data", '5', NULL, NULL);
  tar_add(&tar, "data/layouts", '5', NULL, NULL);
  tar_add(&tar, "data/PaxHeaders.1/standard15.txt", 'x', NULL,
          "30 mtime=1700000000.123456789\n");
  tar_add(&tar, "data/layouts/standard15.txt", '0', NULL, "layout\n");
  // A link's target is relative to the link's directory.
  tar_add(&tar, "data/layouts/copy.txt", '2', "standard15.txt", NULL);
  tar_end(&tar);
  char *data_dir = get_formatted_string("%s/pax", root);
  char *archive = get_formatted_string("%s/pax.tgz", root);
  char *message = unpack(&tar, archive, data_dir);
  assert(!message);
  assert_file_holds(data_dir, "layouts/standard15.txt", "layout\n");
  assert_file_holds(data_dir, "layouts/copy.txt", "layout\n");
  assert(!path_exists(data_dir, "PaxHeaders.1"));
  free(archive);
  free(data_dir);
}

// A GNU long name stops the unpacking rather than leaving the next entry
// under its cut-short name; so does an entry outside data/.
static void test_unreadable_entries_stop_it(const char *root) {
  Tarball long_name = {.length = 0};
  tar_add(&long_name, "data", '5', NULL, NULL);
  tar_add(&long_name, "././@LongLink", 'L', NULL,
          "data/lexica/a_lexicon_name_long_enough_to_need_a_gnu_long_name_"
          "record_in_its_tar_header.kwg");
  tar_add(&long_name, "data/lexica/a_lexicon_name_long_enough_to_need_a_gnu",
          '0', NULL, "kwg");
  tar_end(&long_name);
  char *data_dir = get_formatted_string("%s/long", root);
  char *archive = get_formatted_string("%s/long.tgz", root);
  char *message = unpack(&long_name, archive, data_dir);
  assert(message && strstr(message, "GNU long name"));
  assert(!path_exists(data_dir, "lexica"));
  free(message);
  free(archive);
  free(data_dir);

  Tarball outside = {.length = 0};
  tar_add(&outside, "data", '5', NULL, NULL);
  tar_add(&outside, "etc/passwd", '0', NULL, "x");
  tar_end(&outside);
  data_dir = get_formatted_string("%s/outside", root);
  archive = get_formatted_string("%s/outside.tgz", root);
  message = unpack(&outside, archive, data_dir);
  assert(message && strstr(message, "unexpected entry"));
  free(message);
  free(archive);
  free(data_dir);
}

// The data directory and the directories on the way to it are made, split at
// either separator on Windows; an empty one is refused, not read past.
static void test_data_directories(const char *root) {
  Tarball tar = {.length = 0};
  tar_add(&tar, "data", '5', NULL, NULL);
  tar_add(&tar, "data/layouts/standard15.txt", '0', NULL, "layout\n");
  tar_end(&tar);
  char *archive = get_formatted_string("%s/nested.tgz", root);
  // Backslashes where they separate directories (Windows), '/' elsewhere.
  const char separator = cpath_is_separator('\\') ? '\\' : '/';
  char *data_dir =
      get_formatted_string("%s%cnot%cyet%cthere%cdata", root, separator,
                           separator, separator, separator);
  char *message = unpack(&tar, archive, data_dir);
  assert(!message);
  assert_file_holds(data_dir, "layouts/standard15.txt", "layout\n");
  free(data_dir);

  message = unpack(&tar, archive, "");
  assert(message);
  free(message);
  free(archive);
}

void test_data_fetch(void) {
  char *root = test_platform_make_temporary_directory("data_fetch_test");
  test_pax_headers_are_skipped(root);
  test_unreadable_entries_stop_it(root);
  test_data_directories(root);
  test_platform_remove_directory_tree(root);
  free(root);
}
