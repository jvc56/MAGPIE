#ifndef ARENA_H
#define ARENA_H

#include "../compat/malloc.h"
#include "../util/io.h"

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

static inline Arena *create_arena(size_t initial_capacity, size_t alignment) {
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

static inline void *arena_alloc(Arena *arena, size_t size) {
  if (!arena) {
    return NULL;
  }

  // Ensure 'size' is a multiple of 16 for 16-byte alignment
  if (size % 16 != 0) {
    log_fatal("Allocation size must be a multiple of 16 bytes.");
  }

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
    log_warn("grew arena; new capacity %d", new_capacity);
  }

  // "Allocate" memory
  void *ptr = arena->memory + arena->size;
  arena->size += size;

  return ptr;
}

static inline void arena_dealloc(Arena *arena, size_t size) {
  if (!arena) {
    return;
  }

  // Ensure 'size' is a multiple of 16 for 16-byte alignment
  if (size % 16 != 0) {
    log_fatal("Allocation size must be a multiple of 16 bytes.");
  }
  arena->size -= size;
}

static inline void arena_reset(Arena *arena) {
  if (!arena) {
    return;
  }
  arena->size = 0;
}

static inline void arena_destroy(Arena *arena) {
  if (!arena) {
    return;
  }
  portable_aligned_free(arena->memory);
  free(arena);
}

#endif