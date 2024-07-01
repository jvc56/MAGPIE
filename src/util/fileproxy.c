#include <stdio.h>
#include <stdlib.h>

#include "log.h"
#include "string_util.h"
#include "util.h"

#define MAX_CACHE_SIZE 32
#define MAX_DATA_FILENAME_LENGTH 64

typedef struct FileCacheEntry {
  char filename[MAX_DATA_FILENAME_LENGTH];
  char *raw_data;
  int byte_size;
} FileCacheEntry;

typedef struct FileCache {
  FileCacheEntry entries[MAX_CACHE_SIZE];
  int num_items;
} FileCache;

static FileCache file_cache = {0};

FILE *stream_from_filename(const char *filename) {
  // Look in cache.
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
  stream = fopen(filename, "r");
  return stream;
}

void precache_file_data(const char *filename, char *raw_data, int num_bytes) {
  char *data_copy = malloc_or_die(sizeof(char) * num_bytes);
  memory_copy(data_copy, raw_data, num_bytes);

  string_copy(file_cache.entries[file_cache.num_items].filename, filename);
  file_cache.entries[file_cache.num_items].raw_data = data_copy;
  file_cache.entries[file_cache.num_items].byte_size = num_bytes;
  log_debug("Cached %s (%d) in cache, with a size of %d", filename,
            string_length(filename), num_bytes);
  file_cache.num_items++;
}

void fileproxy_destroy_cache(void) {
  for (int i = 0; i < file_cache.num_items; i++) {
    free(file_cache.entries[i].raw_data);
  }
  file_cache.num_items = 0;
}