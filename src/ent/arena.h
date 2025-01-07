#ifndef ARENA_H
#define ARENA_H

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t *memory;
  size_t size;
  size_t capacity; // Total capacity of the memory block
} Arena;

#define INITIAL_ARENA_CAPACITY (1024 * 1024) // 1 MB

#ifdef _WIN32
#include <malloc.h> // For _aligned_malloc and _aligned_free
#endif

// Portable aligned allocation function
int portable_aligned_alloc(void **ptr, size_t alignment, size_t size) {
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
void portable_aligned_free(void *ptr) {
  if (!ptr)
    return;

#ifdef _WIN32
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

Arena *create_arena(size_t initial_capacity, size_t alignment) {
  // alignment = 16 for our "SmallMove" structure.
  Arena *arena = malloc(sizeof(Arena));
  if (!arena)
    return NULL;

  // Set default capacity if not specified
  arena->capacity =
      initial_capacity > 0 ? initial_capacity : INITIAL_ARENA_CAPACITY;

  // Allocate aligned memory
  if (portable_aligned_alloc((void **)&arena->memory, alignment,
                             arena->capacity) != 0) {
    free(arena);
    return NULL;
  }

  arena->size = 0;
  return arena;
}

void *arena_alloc(Arena *arena, size_t size) {
  if (!arena) {
    return NULL;
  }

  // Ensure 'size' is a multiple of 16 for 16-byte alignment
  assert(size % 16 == 0 && "Allocation size must be a multiple of 16 bytes.");

  // Check if there's enough space; if not, grow the arena
  if (arena->size + size > arena->capacity) {
    // Double the capacity or set to required size, whichever is larger
    size_t new_capacity = arena->capacity * 2;
    if (arena->size + size > new_capacity) {
      new_capacity = arena->size + size;
    }

    uint8_t *new_memory;
    if (portable_aligned_alloc((void **)&new_memory, 16, new_capacity) != 0) {
      return NULL; // Allocation failed
    }

    // Copy existing data to the new memory block and free old memory
    memcpy(new_memory, arena->memory, arena->size);
    portable_aligned_free(arena->memory);

    // Update arena pointers and capacity
    arena->memory = new_memory;
    arena->capacity = new_capacity;
  }

  // "Allocate" memory
  void *ptr = arena->memory + arena->size;
  arena->size += size;

  return ptr;
}

void arena_reset(Arena *arena) {
  if (!arena) {
    return;
  }
  arena->size = 0;
}

void arena_destroy(Arena *arena) {
  if (!arena) {
    return;
  }
  portable_aligned_free(arena->memory);
  free(arena);
}

#endif