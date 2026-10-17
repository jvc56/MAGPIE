#include "data_fetch_test.h"

#include "../src/compat/cfile.h"
#include "../src/compat/cpath.h"
#include "../src/ent/board_layout.h"
#include "../src/ent/data_filepaths.h"
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
  TAR_END_BYTES = 2 * TAR_BLOCK,
  TAR_SIZE_FIELD = 124,
  TAR_CHECKSUM_FIELD = 148,
  MAX_TARBALL_BYTES = 32 * TAR_BLOCK,
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

// Puts `text` in a header field, without the terminating NUL that a field
// filled to its width does not have.
static void tar_text(unsigned char *field, const char *text) {
  for (size_t index = 0; text[index]; index++) {
    field[index] = (unsigned char)text[index];
  }
}

// Fills in `header`'s checksum, as tar does once the rest is written.
static void tar_checksum(unsigned char *header) {
  memset(header + TAR_CHECKSUM_FIELD, ' ', 8);
  unsigned int checksum = 0;
  for (int byte_idx = 0; byte_idx < TAR_BLOCK; byte_idx++) {
    checksum += header[byte_idx];
  }
  tar_octal(header + TAR_CHECKSUM_FIELD, 7, checksum);
}

// Appends a ustar header for `name` of `type` and `size` bytes, linking to
// `link` (or not, for NULL), then `contents`, padded to a whole block.
// Returns the header, for tests to spoil.
static unsigned char *tar_add(Tarball *tar, const char *name, char type,
                              const char *link, const char *contents) {
  const size_t size = contents ? strlen(contents) : 0;
  const size_t padded = (size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
  assert(tar->length + TAR_BLOCK + padded <= MAX_TARBALL_BYTES);
  unsigned char *header = tar->bytes + tar->length;
  memset(header, 0, TAR_BLOCK);
  tar_text(header, name);
  tar_octal(header + 100, 8, type == '5' ? 0755 : 0644);
  tar_octal(header + 108, 8, 0);
  tar_octal(header + 116, 8, 0);
  tar_octal(header + TAR_SIZE_FIELD, 12, size);
  tar_octal(header + 136, 12, 0);
  header[156] = (unsigned char)type;
  if (link) {
    tar_text(header + 157, link);
  }
  tar_text(header + 257, "ustar");
  tar_text(header + 263, "00");
  tar_checksum(header);
  tar->length += TAR_BLOCK;
  memset(tar->bytes + tar->length, 0, padded);
  if (size > 0) {
    memcpy(tar->bytes + tar->length, contents, size);
  }
  tar->length += padded;
  return header;
}

// Appends one zero block: half the end of an archive.
static void tar_zero_block(Tarball *tar) {
  assert(tar->length + TAR_BLOCK <= MAX_TARBALL_BYTES);
  memset(tar->bytes + tar->length, 0, TAR_BLOCK);
  tar->length += TAR_BLOCK;
}

static void tar_end(Tarball *tar) {
  assert(tar->length + TAR_END_BYTES <= MAX_TARBALL_BYTES);
  memset(tar->bytes + tar->length, 0, TAR_END_BYTES);
  tar->length += TAR_END_BYTES;
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
  free(message);
  assert_file_holds(data_dir, "layouts/standard15.txt", "layout\n");
  assert_file_holds(data_dir, "layouts/copy.txt", "layout\n");
  assert(!path_exists(data_dir, "PaxHeaders.1"));
  free(archive);
  free(data_dir);
}

// Unpacks `tar` into a directory of its own under `root` named `name`, and
// checks that it stops with a message holding `expected`.
static void assert_refused(const Tarball *tar, const char *root,
                           const char *name, const char *expected) {
  char *data_dir = get_formatted_string("%s/%s", root, name);
  char *archive = get_formatted_string("%s/%s.tgz", root, name);
  char *message = unpack(tar, archive, data_dir);
  if (!message || !strstr(message, expected)) {
    log_fatal("unpacking %s said '%s', not '%s'", name,
              message ? message : "nothing", expected);
  }
  free(message);
  free(archive);
  free(data_dir);
}

// What MAGPIE-DATA's tarballs never hold, and this reader therefore does not
// read, stops the unpacking rather than unpacking something else: a hard link
// (which GNU tar writes for a file with two names) would otherwise be
// missing, and a pax path or a base-256 size would be read wrong.
static void test_unsupported_entries_stop_it(const char *root) {
  Tarball hard_link = {.length = 0};
  tar_add(&hard_link, "data", '5', NULL, NULL);
  tar_add(&hard_link, "data/a.txt", '0', NULL, "a");
  tar_add(&hard_link, "data/b.txt", '1', "data/a.txt", NULL);
  tar_end(&hard_link);
  assert_refused(&hard_link, root, "hard", "hard link");

  Tarball pax_path = {.length = 0};
  tar_add(&pax_path, "data", '5', NULL, NULL);
  tar_add(&pax_path, "data/PaxHeaders.1/a.txt", 'x', NULL,
          "19 path=data/b.txt\n");
  tar_add(&pax_path, "data/a.txt", '0', NULL, "a");
  tar_end(&pax_path);
  assert_refused(&pax_path, root, "paxpath", "pax path");

  Tarball base256 = {.length = 0};
  tar_add(&base256, "data", '5', NULL, NULL);
  unsigned char *header = tar_add(&base256, "data/a.txt", '0', NULL, "a");
  memset(header + TAR_SIZE_FIELD, 0, 12);
  header[TAR_SIZE_FIELD] = 0x80;
  header[TAR_SIZE_FIELD + 11] = 1;
  tar_checksum(header);
  tar_end(&base256);
  assert_refused(&base256, root, "base256", "size MAGPIE cannot read");

  Tarball checksum = {.length = 0};
  tar_add(&checksum, "data", '5', NULL, NULL);
  header = tar_add(&checksum, "data/a.txt", '0', NULL, "a");
  header[0] = 'D';
  tar_end(&checksum);
  assert_refused(&checksum, root, "checksum", "checksum is wrong");

  // One zero block is not the end; one inside the archive is corruption.
  Tarball zero_block = {.length = 0};
  tar_add(&zero_block, "data", '5', NULL, NULL);
  tar_zero_block(&zero_block);
  tar_add(&zero_block, "data/a.txt", '0', NULL, "a");
  tar_end(&zero_block);
  assert_refused(&zero_block, root, "zero", "zero block");

  // A link is written as a copy of a file the tarball holds, never of
  // whatever is on disk where it points.
  Tarball dangling = {.length = 0};
  tar_add(&dangling, "data", '5', NULL, NULL);
  tar_add(&dangling, "data/b.txt", '2', "a.txt", NULL);
  tar_end(&dangling);
  assert_refused(&dangling, root, "dangling", "not a file in it");
}

// Creates the directory `path`, which must not exist yet.
static void make_directory(const char *path) {
  if (cfile_mkdir(path) != 0) {
    log_fatal("cannot create %s", path);
  }
}

// Unpacking over an earlier extraction replaces its links rather than
// writing through them, as tar does: a data directory made by
// download_data.sh (GNU tar makes real links) or a developer's links into
// another checkout keep what they point to.
static void test_existing_links_are_replaced(const char *root) {
  char *outside = get_formatted_string("%s/elsewhere", root);
  char *outside_file = get_formatted_string("%s/victim.txt", outside);
  char *data_dir = get_formatted_string("%s/linked", root);
  char *lexica = get_formatted_string("%s/lexica", data_dir);
  char *file_link = get_formatted_string("%s/lexica/X.kwg", data_dir);
  char *directory_link = get_formatted_string("%s/layouts", data_dir);
  make_directory(outside);
  make_directory(data_dir);
  make_directory(lexica);
  ErrorStack *error_stack = error_stack_create();
  write_string_to_file(outside_file, "wb", "victim", error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  if (!test_platform_symlink("../../elsewhere/victim.txt", file_link) ||
      !test_platform_symlink("../elsewhere", directory_link)) {
    printf("skipping the existing-link cases: this process cannot make "
           "symlinks\n");
  } else {
    Tarball tar = {.length = 0};
    tar_add(&tar, "data", '5', NULL, NULL);
    tar_add(&tar, "data/layouts", '5', NULL, NULL);
    tar_add(&tar, "data/layouts/standard15.txt", '0', NULL, "layout\n");
    tar_add(&tar, "data/lexica", '5', NULL, NULL);
    tar_add(&tar, "data/lexica/X.kwg", '0', NULL, "new");
    tar_add(&tar, "data/lexica/Y.kwg", '2', "X.kwg", NULL);
    tar_end(&tar);
    char *archive = get_formatted_string("%s/linked.tgz", root);
    char *message = unpack(&tar, archive, data_dir);
    assert(!message);
    free(message);
    free(archive);
    assert_file_holds(outside, "victim.txt", "victim");
    assert(!path_exists(outside, "standard15.txt"));
    assert(!test_platform_is_symlink(file_link));
    assert(!test_platform_is_symlink(directory_link));
    assert(cfile_is_directory(directory_link));
    assert_file_holds(data_dir, "layouts/standard15.txt", "layout\n");
    assert_file_holds(data_dir, "lexica/X.kwg", "new");
    assert_file_holds(data_dir, "lexica/Y.kwg", "new");
  }
  free(directory_link);
  free(file_link);
  free(lexica);
  free(data_dir);
  free(outside_file);
  free(outside);
}

// Writes `contents` to `name` in `directory`.
static void write_file_in(const char *directory, const char *name,
                          const char *contents) {
  char *path = get_formatted_string("%s/%s", directory, name);
  ErrorStack *error_stack = error_stack_create();
  write_string_to_file(path, "wb", contents, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  free(path);
}

// What a directory's contents and version marker say at startup: a fetch
// that never finished is not taken for data, whatever files it left.
static void test_data_states(const char *root) {
  char *data_dir = get_formatted_string("%s/states", root);
  char *layouts = get_formatted_string("%s/layouts", data_dir);
  make_directory(data_dir);
  assert(data_fetch_get_state(data_dir) == DATA_FETCH_STATE_MISSING);
  write_file_in(data_dir, DATA_FETCH_VERSION_FILENAME, "incomplete\n");
  assert(data_fetch_get_state(data_dir) == DATA_FETCH_STATE_INCOMPLETE);

  make_directory(layouts);
  char *layout_name = board_layout_get_default_name();
  char *layout_path =
      get_filepath(data_dir, layout_name, DATA_FILEPATH_TYPE_LAYOUT);
  ErrorStack *error_stack = error_stack_create();
  write_string_to_file(layout_path, "wb", "layout\n", error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  assert(data_fetch_get_state(data_dir) == DATA_FETCH_STATE_INCOMPLETE);
  write_file_in(data_dir, DATA_FETCH_VERSION_FILENAME, "19990101\n");
  assert(data_fetch_get_state(data_dir) == DATA_FETCH_STATE_OUTDATED);
  write_file_in(data_dir, DATA_FETCH_VERSION_FILENAME,
                DATA_FETCH_VERSION "\r\n");
  assert(data_fetch_get_state(data_dir) == DATA_FETCH_STATE_CURRENT);
  char *marker =
      get_formatted_string("%s/%s", data_dir, DATA_FETCH_VERSION_FILENAME);
  if (remove(marker) != 0) {
    log_fatal("cannot remove %s", marker);
  }
  assert(data_fetch_get_state(data_dir) == DATA_FETCH_STATE_UNVERSIONED);
  free(marker);
  free(layout_path);
  free(layout_name);
  free(layouts);
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
  test_unsupported_entries_stop_it(root);
  test_existing_links_are_replaced(root);
  test_data_states(root);
  test_data_directories(root);
  test_platform_remove_directory_tree(root);
  free(root);
}
