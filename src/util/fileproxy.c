#include "fileproxy.h"

#include "../compat/cfile.h"
#include "../compat/cmemfile.h"
#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "io_util.h"
#include "string_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FileCacheEntry {
  char *filename;
  char *raw_data;
  int byte_size;
} FileCacheEntry;

typedef struct FileCache {
  FileCacheEntry *entries;
  int num_items;
  char **retired_data;
  int num_retired;
} FileCache;

static FileCache file_cache = {0};
static cpthread_mutex_t cache_mutex = CPTHREAD_MUTEX_INITIALIZER;

// Relative paths with a leading ./ address the same cached file.
static const char *cache_filename(const char *filename) {
  while (filename && has_prefix("./", filename)) {
    filename += 2;
  }
  return filename;
}

FILE *stream_from_filename(const char *filename, ErrorStack *error_stack) {
  // Look in cache.
  if (!filename) {
    log_fatal("attempted to get stream for null filename");
    return NULL;
  }
  const char *original_filename = filename;
  filename = cache_filename(filename);
  cpthread_mutex_lock(&cache_mutex);
  for (int entry_idx = 0; entry_idx < file_cache.num_items; entry_idx++) {
    if (strings_equal(file_cache.entries[entry_idx].filename, filename)) {
      log_debug("Found %s in cache...", filename);
      FILE *stream =
          cmemfile_open(file_cache.entries[entry_idx].raw_data,
                        (size_t)file_cache.entries[entry_idx].byte_size);
      cpthread_mutex_unlock(&cache_mutex);
      return stream;
    }
  }
  log_debug("%s not found in cache (size %d), opening", filename,
            file_cache.num_items);
  cpthread_mutex_unlock(&cache_mutex);
  FILE *stream;
  stream = fopen_safe(original_filename, "rb", error_stack);
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
    return;
  }
  char *data_copy = malloc_or_die(sizeof(char) * num_bytes);
  memcpy(data_copy, raw_data, num_bytes);

  filename = cache_filename(filename);
  cpthread_mutex_lock(&cache_mutex);
  int entry_idx = 0;
  while (entry_idx < file_cache.num_items &&
         !strings_equal(file_cache.entries[entry_idx].filename, filename)) {
    entry_idx++;
  }
  if (entry_idx == file_cache.num_items) {
    file_cache.entries = realloc_or_die(
        file_cache.entries, (size_t)(entry_idx + 1) * sizeof(FileCacheEntry));
    file_cache.entries[entry_idx].filename = string_duplicate(filename);
    file_cache.num_items++;
  } else if (file_cache.entries[entry_idx].byte_size == num_bytes &&
             memcmp(file_cache.entries[entry_idx].raw_data, raw_data,
                    (size_t)num_bytes) == 0) {
    // Re-fetching unchanged data must not retain another full lexicon copy.
    free(data_copy);
    cpthread_mutex_unlock(&cache_mutex);
    return;
  } else {
    // Existing fmemopen streams borrow these bytes until they are closed.
    // Keep replaced buffers alive until the caller tears down the cache.
    file_cache.retired_data =
        realloc_or_die(file_cache.retired_data,
                       (size_t)(file_cache.num_retired + 1) * sizeof(char *));
    file_cache.retired_data[file_cache.num_retired++] =
        file_cache.entries[entry_idx].raw_data;
  }
  file_cache.entries[entry_idx].raw_data = data_copy;
  file_cache.entries[entry_idx].byte_size = num_bytes;
  cpthread_mutex_unlock(&cache_mutex);
}

void fileproxy_destroy_cache(void) {
  cpthread_mutex_lock(&cache_mutex);
  for (int entry_idx = 0; entry_idx < file_cache.num_items; entry_idx++) {
    free(file_cache.entries[entry_idx].filename);
    free(file_cache.entries[entry_idx].raw_data);
  }
  free(file_cache.entries);
  file_cache.entries = NULL;
  file_cache.num_items = 0;
  for (int entry_idx = 0; entry_idx < file_cache.num_retired; entry_idx++) {
    free(file_cache.retired_data[entry_idx]);
  }
  free(file_cache.retired_data);
  file_cache.retired_data = NULL;
  file_cache.num_retired = 0;
  cpthread_mutex_unlock(&cache_mutex);
}

bool fileproxy_file_exists(const char *filename) {
  const char *original_filename = filename;
  filename = cache_filename(filename);
  cpthread_mutex_lock(&cache_mutex);
  // Check cache first
  for (int entry_idx = 0; entry_idx < file_cache.num_items; entry_idx++) {
    if (strings_equal(file_cache.entries[entry_idx].filename, filename)) {
      cpthread_mutex_unlock(&cache_mutex);
      return true;
    }
  }
  cpthread_mutex_unlock(&cache_mutex);
  // Fall back to filesystem check
  return cfile_access(original_filename, CFILE_ACCESS_READ);
}