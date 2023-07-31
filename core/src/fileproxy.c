#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fileproxy.h"
#include "log.h"

static FileCache file_cache = {0};

FILE *stream_from_filename(const char *filename) {
  // Look in cache.
  for (int i = 0; i < file_cache.num_items; i++) {
    if (strcmp(file_cache.entries[i].filename, filename) == 0) {
      log_debug("Found %s in cache...", filename);
      return fmemopen(file_cache.entries[i].raw_data,
                      file_cache.entries[i].byte_size, "r");
    }
  }
  log_debug("%s not found in cache, opening", filename);
  FILE *stream;
  stream = fopen(filename, "r");
  return stream;
}

void precache_file_data(const char *filename, char *raw_data, int num_bytes) {
  char *data_copy = malloc(sizeof(char) * num_bytes);
  memcpy(data_copy, raw_data, num_bytes);

  strcpy(file_cache.entries[file_cache.num_items].filename, filename);
  file_cache.entries[file_cache.num_items].raw_data = data_copy;
  file_cache.entries[file_cache.num_items].byte_size = num_bytes;
  log_debug("Cached %s (%d) in cache, with a size of %d", filename,
            strlen(filename), num_bytes);
  file_cache.num_items++;
}

void destroy_cache() {
  for (int i = 0; i < file_cache.num_items; i++) {
    free(file_cache.entries[i].raw_data);
  }
  file_cache.num_items = 0;
}