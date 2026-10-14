#include "data_fetch.h"

#include "../compat/cfile.h"
#include "../compat/chttp.h"
#include "../ent/board_layout.h"
#include "../ent/data_filepaths.h"
#include "../util/hash.h"
#include "../util/http_client.h"
#include "../util/io_util.h"
#include "../util/json.h"
#include "../util/string_util.h"
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // The SHA-256 of a file, in hex.
  DATA_FETCH_SHA256_HEX_LENGTH = 64,
};

static void data_fetch_report(data_fetch_report_t report, void *context,
                              char *message) {
  if (report) {
    report(context, message);
  }
  free(message);
}

// Whether `path` is a relative, '/'-separated path that stays inside the
// data directory: no leading '/', no "." or ".." components, no backslashes
// or drive letters, and only the characters data file names use.
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

// Creates every directory on the way to the file at `path`.
static void data_fetch_make_parent_directories(const char *path,
                                               ErrorStack *error_stack) {
  char *prefix = string_duplicate(path);
  for (char *character = prefix + 1; *character; character++) {
    if (*character != '/') {
      continue;
    }
    *character = '\0';
    if (!cfile_is_directory(prefix) && cfile_mkdir(prefix) != 0 &&
        errno != EEXIST) {
      error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                       get_formatted_string("could not create directory %s: %s",
                                            prefix, strerror(errno)));
      break;
    }
    *character = '/';
  }
  free(prefix);
}

// Writes `length` bytes to `path` in one step.
static void data_fetch_write_file(const char *path, const char *bytes,
                                  size_t length, ErrorStack *error_stack) {
  data_fetch_make_parent_directories(path, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  CFileAtomicWrite atomic_write;
  FILE *stream = cfile_atomic_write_begin(&atomic_write, path);
  if (!stream) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_WRITE_ERROR,
        get_formatted_string("could not create %s: %s", path, strerror(errno)));
    return;
  }
  bool written = length == 0 || fwrite(bytes, 1, length, stream) == length;
  int error_number = written ? 0 : errno;
  if (!cfile_atomic_write_end(&atomic_write, written) && written) {
    error_number = errno;
    written = false;
  }
  if (!written) {
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     get_formatted_string("could not write %s: %s", path,
                                          strerror(error_number)));
  }
}

// Whether the file at `path` exists with SHA-256 `sha256`.
static bool data_fetch_file_matches(const char *path, const char *sha256) {
  if (!cfile_access(path, CFILE_ACCESS_EXISTS)) {
    return false;
  }
  ErrorStack *error_stack = error_stack_create();
  char *existing = sha256_hash_file(path, error_stack);
  const bool matches =
      error_stack_is_empty(error_stack) && strings_iequal(existing, sha256);
  free(existing);
  error_stack_destroy(error_stack);
  return matches;
}

// Downloads one listed file into `destination`, unless it is already there.
static void data_fetch_file(HttpClient *client, const JsonValue *file,
                            const char *destination, int file_idx,
                            int file_count, data_fetch_report_t report,
                            void *report_context, ErrorStack *error_stack) {
  const char *url = json_get_string(file, "url", error_stack);
  const char *sha256 = json_get_string(file, "sha256", error_stack);
  const int64_t bytes = json_get_int(file, "bytes", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  if (string_length(sha256) != DATA_FETCH_SHA256_HEX_LENGTH || bytes < 0) {
    error_stack_push(error_stack, ERROR_STATUS_DATA_FETCH_MANIFEST_INVALID,
                     get_formatted_string("data manifest entry for %s has an "
                                          "invalid size or SHA-256",
                                          destination));
    return;
  }
  if (data_fetch_file_matches(destination, sha256)) {
    data_fetch_report(report, report_context,
                      get_formatted_string("[%d/%d] %s is up to date",
                                           file_idx + 1, file_count,
                                           destination));
    return;
  }
  data_fetch_report(
      report, report_context,
      get_formatted_string("[%d/%d] downloading %s (%" PRId64 " bytes)",
                           file_idx + 1, file_count, destination, bytes));
  ChttpResponse response = {0};
  http_client_get(client, url, &response, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  if (response.status_code != 200) {
    error_stack_push(error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
                     get_formatted_string("downloading %s returned HTTP %ld",
                                          url, response.status_code));
    chttp_response_destroy(&response);
    return;
  }
  char *received = sha256_hash_bytes(response.body, response.body_length);
  if ((int64_t)response.body_length != bytes ||
      !strings_iequal(received, sha256)) {
    error_stack_push(error_stack, ERROR_STATUS_DATA_FETCH_FILE_MISMATCH,
                     get_formatted_string(
                         "%s is not the file the data manifest lists "
                         "(%zu bytes, SHA-256 %s; expected %" PRId64
                         " bytes, %s)",
                         url, response.body_length, received, bytes, sha256));
  } else {
    data_fetch_write_file(destination, response.body, response.body_length,
                          error_stack);
  }
  free(received);
  chttp_response_destroy(&response);
}

// Writes `destination` as a copy of the already fetched `source`.
static void data_fetch_copy(const char *source, const char *destination,
                            ErrorStack *error_stack) {
  FILE *stream = fopen_safe(source, "rb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  int64_t size = 0;
  if (!cfile_stream_size(stream, &size)) {
    fclose_or_die(stream);
    error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                     get_formatted_string("could not read %s", source));
    return;
  }
  char *bytes = malloc_or_die((size_t)size + 1);
  const bool read = fread(bytes, 1, (size_t)size, stream) == (size_t)size;
  fclose_or_die(stream);
  if (!read) {
    error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                     get_formatted_string("could not read %s", source));
  } else {
    data_fetch_write_file(destination, bytes, (size_t)size, error_stack);
  }
  free(bytes);
}

void data_fetch(const char *manifest_url, const char *data_dir,
                data_fetch_report_t report, void *report_context,
                ErrorStack *error_stack) {
  if (!chttp_is_available()) {
    error_stack_push(
        error_stack, ERROR_STATUS_HTTP_UNAVAILABLE,
        string_duplicate("this build of MAGPIE cannot download files"));
    return;
  }
  HttpClient *client = http_client_create("", NULL, NULL);
  data_fetch_report(
      report, report_context,
      get_formatted_string("fetching the data manifest %s", manifest_url));
  ChttpResponse response = {0};
  http_client_get(client, manifest_url, &response, error_stack);
  if (error_stack_is_empty(error_stack) && response.status_code != 200) {
    error_stack_push(error_stack, ERROR_STATUS_HTTP_REQUEST_FAILED,
                     get_formatted_string("fetching %s returned HTTP %ld",
                                          manifest_url, response.status_code));
  }
  const JsonValue *manifest = NULL;
  if (error_stack_is_empty(error_stack)) {
    manifest = json_parse(response.body, error_stack);
  }
  chttp_response_destroy(&response);
  const JsonValue *files = manifest ? json_object_get(manifest, "files") : NULL;
  if (error_stack_is_empty(error_stack) && !json_is_array(files)) {
    error_stack_push(error_stack, ERROR_STATUS_DATA_FETCH_MANIFEST_INVALID,
                     get_formatted_string("%s is not a data manifest: it has "
                                          "no \"files\" list",
                                          manifest_url));
  }
  const int file_count = files ? json_array_length(files) : 0;
  // Downloads first, then copies, which may name any downloaded file.
  for (int pass = 0; pass < 2 && error_stack_is_empty(error_stack); pass++) {
    for (int file_idx = 0;
         file_idx < file_count && error_stack_is_empty(error_stack);
         file_idx++) {
      const JsonValue *file = json_array_get(files, file_idx);
      const char *path = json_get_string(file, "path", error_stack);
      const char *copy_of = json_get_string_or_null(file, "copy_of");
      if (!error_stack_is_empty(error_stack)) {
        break;
      }
      if (!data_fetch_is_safe_path(path) ||
          (copy_of && !data_fetch_is_safe_path(copy_of))) {
        error_stack_push(
            error_stack, ERROR_STATUS_DATA_FETCH_MANIFEST_INVALID,
            get_formatted_string("data manifest names an unsafe path: %s",
                                 path ? path : "(none)"));
        break;
      }
      char *destination = get_formatted_string("%s/%s", data_dir, path);
      if (pass == 0 && !copy_of) {
        data_fetch_file(client, file, destination, file_idx, file_count, report,
                        report_context, error_stack);
      } else if (pass == 1 && copy_of) {
        char *source = get_formatted_string("%s/%s", data_dir, copy_of);
        data_fetch_copy(source, destination, error_stack);
        free(source);
      }
      free(destination);
    }
  }
  if (error_stack_is_empty(error_stack)) {
    data_fetch_report(report, report_context,
                      get_formatted_string("data is ready in %s", data_dir));
  }
  json_destroy(manifest);
  http_client_destroy(client);
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
