#ifndef CMAP_H
#define CMAP_H

#include <stdbool.h>
#include <stddef.h>

// A file mapped read-only into memory: mmap on POSIX, a file mapping view
// on Windows.
typedef struct CMap {
  const void *data;
  size_t size;
} CMap;

// Maps all of the file at `path` (UTF-8). With `random_access`, tells the
// system lookups will jump around, so it should not read ahead (madvise
// MADV_RANDOM; Windows has no equivalent and ignores it). Returns false with
// errno set on failure, including for an empty file, which cannot be mapped.
bool cmap_open(CMap *map, const char *path, bool random_access);

// Unmaps what cmap_open mapped. Does nothing to a map that was never opened
// or failed to open.
void cmap_close(CMap *map);

#endif
