#include "fileproxy.h"

#include "io_util.h"
#include "string_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct FileCacheEntry {
  char *filename;
  char *raw_data;
  int byte_size;
} FileCacheEntry;

typedef struct FileCache {
  FileCacheEntry *entries;
  int num_items;
} FileCache;

static FileCache file_cache = {0};

FILE *stream_from_filename(const char *filename, ErrorStack *error_stack) {
  // Look in cache.
  if (!filename) {
    log_fatal("attempted to get stream for null filename");
  }
  for (int i = 0; i < file_cache.num_items; i++) {
    if (strings_equal(file_cache.entries[i].filename, filename)) {
      log_debug("Found %s in cache...", filename);
      return fmemopen(file_cache.entries[i].raw_data,
                      file_cache.entries[i].byte_size, "r");
    }
  }
  log_debug("%s not found in cache (size %d), opening", filename,
            file_cache.num_items);
  FILE *stream;
  stream = fopen_safe(filename, "r", error_stack);
  return stream;
}

char *fileproxy_get_string_from_filename(const char *filename,
                                         ErrorStack *error_stack) {
  FILE *file = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  return get_string_from_file_handle(file, filename, error_stack);
}

void precache_file_data(const char *filename, const char *raw_data,
                        const int num_bytes) {
  if (!filename || !raw_data || num_bytes < 0) {
    log_fatal("invalid file cache input");
  }
  char *data_copy = malloc_or_die(sizeof(char) * num_bytes);
  memcpy(data_copy, raw_data, num_bytes);

  int index = 0;
  while (index < file_cache.num_items &&
         !strings_equal(file_cache.entries[index].filename, filename)) {
    index++;
  }
  if (index == file_cache.num_items) {
    file_cache.entries = realloc_or_die(
        file_cache.entries, (size_t)(index + 1) * sizeof(FileCacheEntry));
    file_cache.entries[index].filename = string_duplicate(filename);
    file_cache.num_items++;
  } else {
    free(file_cache.entries[index].raw_data);
  }
  file_cache.entries[index].raw_data = data_copy;
  file_cache.entries[index].byte_size = num_bytes;
}

void fileproxy_destroy_cache(void) {
  for (int i = 0; i < file_cache.num_items; i++) {
    free(file_cache.entries[i].filename);
    free(file_cache.entries[i].raw_data);
  }
  free(file_cache.entries);
  file_cache.entries = NULL;
  file_cache.num_items = 0;
}

bool fileproxy_file_exists(const char *filename) {
  // Check cache first
  for (int i = 0; i < file_cache.num_items; i++) {
    if (strings_equal(file_cache.entries[i].filename, filename)) {
      return true;
    }
  }
  // Fall back to filesystem check
  return access(filename, F_OK | R_OK) == 0;
}