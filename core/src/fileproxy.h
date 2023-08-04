#ifndef FILEPROXY_H
#define FILEPROXY_H

#include "constants.h"
#include <stdio.h>

#define MAX_CACHE_SIZE 32

typedef struct FileCacheEntry {
  char filename[MAX_DATA_FILENAME_LENGTH];
  char *raw_data;
  int byte_size;
} FileCacheEntry;

typedef struct FileCache {
  FileCacheEntry entries[MAX_CACHE_SIZE];
  int num_items;
} FileCache;

FILE *stream_from_filename(const char *filename);
void precache_file(const char *filename);
void destroy_cache();

#endif