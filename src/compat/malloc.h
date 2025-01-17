#ifndef COMPAT_MALLOC_H
#define COMPAT_MALLOC_H

#include <stdlib.h>

#ifdef _WIN32
#include <malloc.h> // For _aligned_malloc and _aligned_free
#endif

// Portable aligned allocation function
static inline int portable_aligned_alloc(void **ptr, size_t alignment,
                                         size_t size) {
  if (!ptr) {
    return -1; // Invalid pointer
  }

#ifdef _WIN32
  // Windows-specific aligned allocation
  *ptr = _aligned_malloc(size, alignment);
  if (*ptr == NULL) {
    return -1; // Allocation failed
  }
  return 0; // Success
#else
  // POSIX-compliant aligned allocation
  // posix_memalign returns 0 on success, or an error number on failure
  return posix_memalign(ptr, alignment, size);
#endif
}

// Portable aligned free function
static inline void portable_aligned_free(void *ptr) {
  if (!ptr)
    return;

#ifdef _WIN32
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

#endif
