#ifndef FNV_H
#define FNV_H

#include <stdint.h>

// 64-bit FNV-1a hash constants (the standard offset basis and prime). These
// are too large to be enumeration constants (enumerators have type int), so
// they are named #defines rather than an enum.
#define FNV_64_OFFSET_BASIS 14695981039346656037ULL
#define FNV_64_PRIME 1099511628211ULL

// One FNV-1a step: fold a 64-bit value into the running hash. Seed the running
// hash with FNV_64_OFFSET_BASIS before the first step.
static inline uint64_t fnv64a_step(uint64_t hash, uint64_t value) {
  hash ^= value;
  hash *= FNV_64_PRIME;
  return hash;
}

#endif
