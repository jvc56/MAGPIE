#include "data_fetch.h"

#include "../compat/cfile.h"
#include "../compat/chttp.h"
#include "../compat/cpath.h"
#include "../ent/board_layout.h"
#include "../ent/data_filepaths.h"
#include "../util/gzip_reader.h"
#include "../util/hash.h"
#include "../util/http_client.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // ustar (POSIX.1-1988) headers and records.
  TAR_BLOCK_BYTES = 512,
  TAR_NAME_OFFSET = 0,
  TAR_NAME_BYTES = 100,
  TAR_SIZE_OFFSET = 124,
  TAR_SIZE_BYTES = 12,
  TAR_TYPE_OFFSET = 156,
  TAR_LINK_OFFSET = 157,
  TAR_LINK_BYTES = 100,
  TAR_PREFIX_OFFSET = 345,
  TAR_PREFIX_BYTES = 155,
  // Two zero blocks end the archive.
  TAR_END_ZERO_BLOCKS = 2,
  // Room on disk a fetch asks for: the compressed tarball while it unpacks
  // (195 MB) and its contents (270 MB), and some to spare.
  DATA_FETCH_MIN_FREE_BYTES = 600 * 1048576,
  DATA_FETCH_COPY_BUFFER_BYTES = 65536,
  DATA_FETCH_MB = 1024 * 1024,
};

typedef struct DataFetchChunk {
  const char *suffix;
  int64_t bytes;
  const char *sha256;
} DataFetchChunk;

// The chunks of data-DATA_FETCH_VERSION.tgz, in order. Update with
// DATA_FETCH_VERSION (sha256sum data-<version>.tgz.*).
static const DataFetchChunk data_fetch_chunks[] = {
    {"aa", 41943040,
     "819c68e6dd41d9d3dbe75176022a097711af5d9567ddbf2c01fc6c461323fc69"},
    {"ab", 41943040,
     "add6eafbec7415f7d5e6e4c31c462981df5242a7eb3ec8459e2cb400b9d55080"},
    {"ac", 41943040,
     "847234799e36bcb54fe13c81660cd7ba87e7924aee20612eca4100496a9aec55"},
    {"ad", 41943040,
     "041bef46e75f1f6c8fa07403927213591db596a168f81bbf1a3d39e19bffbf09"},
    {"ae", 27592111,
     "08a9b7d5571d16d713f15034dfbc0147be846bd28957e66c0bcc5e504a47eedb"},
};

static void data_fetch_report(data_fetch_report_t report, void *context,
                              char *message) {
  if (report) {
    report(context, message);
  }
  free(message);
}

// Whether `path` is a relative, '/'-separated path that stays where it is put:
// no leading '/', no empty, "." or ".." components, and only the characters
// data file names use.
static bool data_fetch_is_safe_path(const char *path) {
  if (!path || !path[0] || path[0] == '/') {
    return false;
  }
  const char *component = path;
  for (const char *character = path;; character++) {
    if (*character == '/' || *character == '\0') {
      const size_t length = (size_t)(character - component);
      if (length == 0 || (length == 1 && component[0] == '.') ||
          (length == 2 && component[0] == '.' && component[1] == '.')) {
        return false;
      }
      if (*character == '\0') {
        return true;
      }
      component = character + 1;
      continue;
    }
    const char c = *character;
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                         c == '-';
    if (!allowed) {
      return false;
    }
  }
}

// Creates `directory` and every directory on the way to it, splitting it at
// either separator on Windows. Works back from the end, so a part that
// already exists and cannot be made (a drive, "\\server\share") is never
// made.
static bool data_fetch_make_directories(const char *directory) {
  if (!directory[0]) {
    errno = ENOENT;
    return false;
  }
  if (cfile_is_directory(directory)) {
    return true;
  }
  size_t parent_length = strlen(directory);
  while (parent_length > 0 &&
         !cpath_is_separator(directory[parent_length - 1])) {
    parent_length--;
  }
  while (parent_length > 0 &&
         cpath_is_separator(directory[parent_length - 1])) {
    parent_length--;
  }
  if (parent_length > 0) {
    char *parent = get_formatted_string("%.*s", (int)parent_length, directory);
    const bool made_parent = data_fetch_make_directories(parent);
    free(parent);
    if (!made_parent) {
      return false;
    }
  }
  return cfile_mkdir(directory) == 0 ||
         (errno == EEXIST && cfile_is_directory(directory));
}

static bool data_fetch_make_parent_directories(const char *path) {
  char *directory = string_duplicate(path);
  char *slash = strrchr(directory, '/');
  bool made = true;
  if (slash && slash != directory) {
    *slash = '\0';
    made = data_fetch_make_directories(directory);
  }
  free(directory);
  return made;
}

// Writes `destination` as a copy of `source`, in one step.
static void data_fetch_copy(const char *source, const char *destination,
                            ErrorStack *error_stack) {
  FILE *stream = fopen_safe(source, "rb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  CFileAtomicWrite atomic_write;
  FILE *output = cfile_atomic_write_begin(&atomic_write, destination);
  if (!output) {
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     get_formatted_string("could not create %s: %s",
                                          destination, strerror(errno)));
    fclose_or_die(stream);
    return;
  }
  char *buffer = malloc_or_die(DATA_FETCH_COPY_BUFFER_BYTES);
  bool copied = true;
  size_t count;
  while (copied &&
         (count = fread(buffer, 1, DATA_FETCH_COPY_BUFFER_BYTES, stream)) > 0) {
    copied = fwrite(buffer, 1, count, output) == count;
  }
  free(buffer);
  copied = copied && !ferror(stream);
  fclose_or_die(stream);
  if (!cfile_atomic_write_end(&atomic_write, copied) || !copied) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_WRITE_ERROR,
        get_formatted_string("could not copy %s to %s", source, destination));
  }
}

// Unpacks a tar stream (what MAGPIE-DATA's tarballs hold: regular files,
// directories and symlinks under data/) into a data directory, a decompressed
// window at a time.
typedef struct TarExtractor {
  const char *data_dir;
  unsigned char header[TAR_BLOCK_BYTES];
  size_t header_used;
  // Bytes of the current entry's contents still to come, and of the padding
  // after them.
  uint64_t remaining;
  uint64_t padding;
  bool writing;
  CFileAtomicWrite file;
  char *file_path;
  int zero_blocks;
  int files;
  // Symlinks, written as copies of their targets once everything is out.
  StringList *link_paths;
  StringList *link_targets;
  ErrorStack *error_stack;
} TarExtractor;

// The field at `offset`, up to `bytes` long, NUL-terminated or not.
static char *tar_field(const unsigned char *header, size_t offset,
                       size_t bytes) {
  size_t length = 0;
  while (length < bytes && header[offset + length] != '\0') {
    length++;
  }
  char *field = malloc_or_die(length + 1);
  memcpy(field, header + offset, length);
  field[length] = '\0';
  return field;
}

static uint64_t tar_size(const unsigned char *header) {
  uint64_t size = 0;
  for (size_t index = 0; index < TAR_SIZE_BYTES; index++) {
    const unsigned char digit = header[TAR_SIZE_OFFSET + index];
    if (digit >= '0' && digit <= '7') {
      size = size * 8 + (uint64_t)(digit - '0');
    } else if (digit != ' ' || size != 0) {
      break;
    }
  }
  return size;
}

static bool tar_fail(TarExtractor *tar, char *message) {
  error_stack_push(tar->error_stack, ERROR_STATUS_DATA_FETCH_FILE_MISMATCH,
                   message);
  return false;
}

// Finishes the file being written, committing it if it is complete.
static bool tar_finish_file(TarExtractor *tar, bool commit) {
  if (!tar->writing) {
    return true;
  }
  tar->writing = false;
  const bool written = cfile_atomic_write_end(&tar->file, commit);
  bool finished = true;
  if (!written && commit) {
    finished =
        tar_fail(tar, get_formatted_string("could not write %s: %s",
                                           tar->file_path, strerror(errno)));
  }
  free(tar->file_path);
  tar->file_path = NULL;
  return finished;
}

// Records a symlink at `relative` (within data/) to `target` (relative to
// the link's directory), to be written as a copy.
static bool tar_add_link(TarExtractor *tar, const char *relative,
                         const char *destination) {
  char *target = tar_field(tar->header, TAR_LINK_OFFSET, TAR_LINK_BYTES);
  const char *slash = strrchr(relative, '/');
  char *target_relative =
      slash ? get_formatted_string("%.*s/%s", (int)(slash - relative), relative,
                                   target)
            : string_duplicate(target);
  bool added = true;
  if (!data_fetch_is_safe_path(target_relative)) {
    added =
        tar_fail(tar, get_formatted_string("the data tarball links %s outside "
                                           "its data: %s",
                                           relative, target));
  } else {
    string_list_add_string(tar->link_paths, destination);
    char *target_path =
        get_formatted_string("%s/%s", tar->data_dir, target_relative);
    string_list_add_string(tar->link_targets, target_path);
    free(target_path);
  }
  free(target_relative);
  free(target);
  return added;
}

static bool tar_start_file(TarExtractor *tar, const char *destination,
                           uint64_t size) {
  if (!data_fetch_make_parent_directories(destination) ||
      !cfile_atomic_write_begin(&tar->file, destination)) {
    return tar_fail(tar, get_formatted_string("could not create %s: %s",
                                              destination, strerror(errno)));
  }
  tar->writing = true;
  tar->file_path = string_duplicate(destination);
  tar->files++;
  return size > 0 || tar_finish_file(tar, true);
}

// The entry's path within data/ ("" for data/ itself), or NULL if it is not
// under data/ or is not a safe path.
static const char *tar_relative_path(const char *path) {
  if (strcmp(path, "data") == 0) {
    return "";
  }
  if (strncmp(path, "data/", 5) == 0 && data_fetch_is_safe_path(path + 5)) {
    return path + 5;
  }
  return NULL;
}

static bool tar_start_entry(TarExtractor *tar) {
  bool all_zero = true;
  for (size_t index = 0; index < TAR_BLOCK_BYTES && all_zero; index++) {
    all_zero = tar->header[index] == 0;
  }
  if (all_zero) {
    tar->zero_blocks++;
    return true;
  }
  char *name = tar_field(tar->header, TAR_NAME_OFFSET, TAR_NAME_BYTES);
  char *prefix = tar_field(tar->header, TAR_PREFIX_OFFSET, TAR_PREFIX_BYTES);
  char *path = prefix[0] ? get_formatted_string("%s/%s", prefix, name)
                         : string_duplicate(name);
  free(prefix);
  free(name);
  size_t length = strlen(path);
  while (length > 0 && path[length - 1] == '/') {
    path[--length] = '\0';
  }
  const char type = (char)tar->header[TAR_TYPE_OFFSET];
  const uint64_t size = tar_size(tar->header);
  tar->remaining = size;
  tar->padding = (TAR_BLOCK_BYTES - size % TAR_BLOCK_BYTES) % TAR_BLOCK_BYTES;
  // A pax header, for the next entry ('x') or the whole archive ('g', which
  // git archive writes as pax_global_header), holds only what MAGPIE has no
  // use for, such as times and owners, so it is skipped with its contents
  // whatever it is called. A GNU long name ('L', 'K', called ././@LongLink)
  // renames the next entry, which this reader cannot do, so rather than
  // unpacking that entry under its cut-short name it stops.
  if (type == 'x' || type == 'g') {
    free(path);
    return true;
  }
  if (type == 'L' || type == 'K') {
    free(path);
    return tar_fail(tar, string_duplicate("the data tarball has a GNU long "
                                          "name, which MAGPIE cannot read"));
  }
  // Every other entry is data/ or under it; anything else is not a MAGPIE
  // data tarball.
  const char *relative = tar_relative_path(path);
  if (!relative) {
    const bool started = tar_fail(
        tar, get_formatted_string("the data tarball has an unexpected entry: "
                                  "%s",
                                  path));
    free(path);
    return started;
  }
  char *destination =
      relative[0] ? get_formatted_string("%s/%s", tar->data_dir, relative)
                  : string_duplicate(tar->data_dir);
  bool started = true;
  if (type == '5') {
    started =
        data_fetch_make_directories(destination) ||
        tar_fail(tar, get_formatted_string("could not create directory %s: %s",
                                           destination, strerror(errno)));
  } else if (type == '0' || type == '\0') {
    started = tar_start_file(tar, destination, size);
  } else if (type == '2') {
    started = tar_add_link(tar, relative, destination);
  }
  // Any other type under data/ (a hard link or a device, which MAGPIE-DATA's
  // tarballs do not have) is skipped with its contents.
  free(destination);
  free(path);
  return started;
}

static bool tar_sink(void *context, const unsigned char *bytes, size_t length) {
  TarExtractor *tar = (TarExtractor *)context;
  size_t used = 0;
  while (used < length) {
    if (tar->zero_blocks >= TAR_END_ZERO_BLOCKS) {
      // Past the end of the archive: what follows is tar's padding.
      return true;
    }
    const size_t available = length - used;
    if (tar->remaining > 0) {
      const size_t count =
          tar->remaining < available ? (size_t)tar->remaining : available;
      if (tar->writing &&
          fwrite(bytes + used, 1, count, tar->file.stream) != count) {
        return tar_fail(tar,
                        get_formatted_string("could not write %s: %s",
                                             tar->file_path, strerror(errno)));
      }
      tar->remaining -= count;
      used += count;
      if (tar->remaining == 0 && !tar_finish_file(tar, true)) {
        return false;
      }
    } else if (tar->padding > 0) {
      const size_t count =
          tar->padding < available ? (size_t)tar->padding : available;
      tar->padding -= count;
      used += count;
    } else {
      size_t count = TAR_BLOCK_BYTES - tar->header_used;
      if (count > available) {
        count = available;
      }
      memcpy(tar->header + tar->header_used, bytes + used, count);
      tar->header_used += count;
      used += count;
      if (tar->header_used == TAR_BLOCK_BYTES) {
        tar->header_used = 0;
        if (!tar_start_entry(tar)) {
          return false;
        }
      }
    }
  }
  return true;
}

// Downloads one chunk, checks it, and appends it to `archive`.
static void data_fetch_chunk(HttpClient *client, const char *url,
                             const DataFetchChunk *chunk, FILE *archive,
                             const char *archive_path,
                             ErrorStack *error_stack) {
  ChttpResponse response = {0};
  http_client_get(client, url, &response, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  char *received = sha256_hash_bytes(response.body, response.body_length);
  if (response.status_code != 200) {
    error_stack_push(error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
                     get_formatted_string("downloading %s returned HTTP %ld",
                                          url, response.status_code));
  } else if ((int64_t)response.body_length != chunk->bytes ||
             !strings_iequal(received, chunk->sha256)) {
    error_stack_push(error_stack, ERROR_STATUS_DATA_FETCH_FILE_MISMATCH,
                     get_formatted_string(
                         "%s is not the data this MAGPIE expects (%zu "
                         "bytes, SHA-256 %s; expected %" PRId64 " bytes, %s)",
                         url, response.body_length, received, chunk->bytes,
                         chunk->sha256));
  } else if (fwrite(response.body, 1, response.body_length, archive) !=
             response.body_length) {
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     get_formatted_string("could not write %s: %s",
                                          archive_path, strerror(errno)));
  }
  free(received);
  chttp_response_destroy(&response);
}

// Downloads the chunks one after another into `archive_path`.
static void data_fetch_download(const char *base_url, const char *archive_path,
                                data_fetch_report_t report,
                                void *report_context, ErrorStack *error_stack) {
  FILE *archive = fopen_safe(archive_path, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  HttpClient *client = http_client_create("", NULL, NULL);
  const int chunk_count =
      (int)(sizeof(data_fetch_chunks) / sizeof(data_fetch_chunks[0]));
  for (int chunk_idx = 0;
       chunk_idx < chunk_count && error_stack_is_empty(error_stack);
       chunk_idx++) {
    const DataFetchChunk *chunk = &data_fetch_chunks[chunk_idx];
    char *url = get_formatted_string("%s/data-%s.tgz.%s", base_url,
                                     DATA_FETCH_VERSION, chunk->suffix);
    data_fetch_report(report, report_context,
                      get_formatted_string("downloading the data, part %d of "
                                           "%d (%" PRId64 " MB)",
                                           chunk_idx + 1, chunk_count,
                                           chunk->bytes / DATA_FETCH_MB));
    data_fetch_chunk(client, url, chunk, archive, archive_path, error_stack);
    free(url);
  }
  http_client_destroy(client);
  if (fclose(archive) != 0 && error_stack_is_empty(error_stack)) {
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     get_formatted_string("could not write %s: %s",
                                          archive_path, strerror(errno)));
  }
}

// Unpacks `archive_path` into `data_dir`, then writes its links as copies.
static void data_fetch_unpack(const char *archive_path, const char *data_dir,
                              data_fetch_report_t report, void *report_context,
                              ErrorStack *error_stack) {
  data_fetch_report(
      report, report_context,
      get_formatted_string("unpacking the data into %s", data_dir));
  FILE *archive = fopen_safe(archive_path, "rb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  TarExtractor tar = {.data_dir = data_dir,
                      .link_paths = string_list_create(),
                      .link_targets = string_list_create(),
                      .error_stack = error_stack};
  const bool decompressed = gzip_reader_decompress(archive, tar_sink, &tar);
  fclose_or_die(archive);
  (void)tar_finish_file(&tar, false);
  if (error_stack_is_empty(error_stack) &&
      (!decompressed || tar.zero_blocks < TAR_END_ZERO_BLOCKS)) {
    error_stack_push(error_stack, ERROR_STATUS_DATA_FETCH_FILE_MISMATCH,
                     string_duplicate("the data tarball is corrupt"));
  }
  const int link_count = string_list_get_count(tar.link_paths);
  for (int link_idx = 0;
       link_idx < link_count && error_stack_is_empty(error_stack); link_idx++) {
    data_fetch_copy(string_list_get_string(tar.link_targets, link_idx),
                    string_list_get_string(tar.link_paths, link_idx),
                    error_stack);
  }
  if (error_stack_is_empty(error_stack)) {
    data_fetch_report(
        report, report_context,
        get_formatted_string("unpacked %d files", tar.files + link_count));
  }
  string_list_destroy(tar.link_paths);
  string_list_destroy(tar.link_targets);
}

void data_fetch_unpack_archive(const char *archive_path, const char *data_dir,
                               ErrorStack *error_stack) {
  data_fetch_unpack(archive_path, data_dir, NULL, NULL, error_stack);
}

// Whether `data_dir` records that it holds data version DATA_FETCH_VERSION.
static bool data_fetch_is_current(const char *data_dir) {
  char *path =
      get_formatted_string("%s/%s", data_dir, DATA_FETCH_VERSION_FILENAME);
  bool current = false;
  if (cfile_access(path, CFILE_ACCESS_READ)) {
    ErrorStack *errors = error_stack_create();
    char *version = get_string_from_file(path, errors);
    if (version) {
      trim_whitespace(version);
      current = strings_equal(version, DATA_FETCH_VERSION);
    }
    free(version);
    error_stack_destroy(errors);
  }
  free(path);
  return current && data_fetch_is_data_present(data_dir);
}

void data_fetch(const char *base_url, const char *data_dir,
                data_fetch_report_t report, void *report_context,
                ErrorStack *error_stack) {
  if (data_fetch_is_current(data_dir)) {
    data_fetch_report(report, report_context,
                      get_formatted_string("%s already holds data version %s",
                                           data_dir, DATA_FETCH_VERSION));
    return;
  }
  if (!chttp_is_available()) {
    error_stack_push(
        error_stack, ERROR_STATUS_HTTP_UNAVAILABLE,
        string_duplicate("this build of MAGPIE cannot download files"));
    return;
  }
  if (!data_fetch_make_directories(data_dir)) {
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     get_formatted_string("could not create %s: %s", data_dir,
                                          strerror(errno)));
    return;
  }
  int64_t free_bytes = 0;
  if (cfile_free_bytes(data_dir, &free_bytes) &&
      free_bytes < DATA_FETCH_MIN_FREE_BYTES) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_WRITE_ERROR,
        get_formatted_string("the data needs about %d MB free beside %s, and "
                             "there are %" PRId64 " MB",
                             DATA_FETCH_MIN_FREE_BYTES / DATA_FETCH_MB,
                             data_dir, free_bytes / DATA_FETCH_MB));
    return;
  }
  char *archive_path = get_formatted_string("%s/.data-%s.tgz.download",
                                            data_dir, DATA_FETCH_VERSION);
  data_fetch_download(base_url ? base_url : DATA_FETCH_DEFAULT_BASE_URL,
                      archive_path, report, report_context, error_stack);
  if (error_stack_is_empty(error_stack)) {
    data_fetch_unpack(archive_path, data_dir, report, report_context,
                      error_stack);
  }
  (void)remove(archive_path);
  free(archive_path);
  if (error_stack_is_empty(error_stack)) {
    char *version_path =
        get_formatted_string("%s/%s", data_dir, DATA_FETCH_VERSION_FILENAME);
    write_string_to_file(version_path, "wb", DATA_FETCH_VERSION "\n",
                         error_stack);
    free(version_path);
  }
  if (error_stack_is_empty(error_stack)) {
    data_fetch_report(report, report_context,
                      get_formatted_string("data version %s is ready in %s",
                                           DATA_FETCH_VERSION, data_dir));
  }
}

bool data_fetch_is_data_present(const char *data_dir) {
  char *layout_name = board_layout_get_default_name();
  char *layout_path =
      get_filepath(data_dir, layout_name, DATA_FILEPATH_TYPE_LAYOUT);
  const bool present = cfile_access(layout_path, CFILE_ACCESS_READ);
  free(layout_path);
  free(layout_name);
  return present;
}
