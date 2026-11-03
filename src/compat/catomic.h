#ifndef CATOMIC_H
#define CATOMIC_H

#include <stdbool.h>
#include <stdint.h>

// Atomic operations on plain objects: the objects are read and written as
// ordinary values outside the phases in which threads update them with these
// (phases that thread joins or other synchronization separate), so that
// filling and reading them costs what it costs for any array.

// catomic_max_int32 where the compiler has no atomic operations on plain
// objects: a striped lock guards each slot. Every target MAGPIE builds for
// takes the lock-free path, so this is exposed for its test.
void catomic_max_int32_locked(int32_t *slot, int32_t value);

// Raises *slot to value if value is larger, as one atomic step against other
// threads raising the same slot with catomic_max_int32.
static inline void catomic_max_int32(int32_t *slot, int32_t value) {
#if defined(__GNUC__) || defined(__clang__)
  // GCC's and Clang's __atomic builtins work on objects of any integral type,
  // which _Atomic types do not promise to share a layout with.
  int32_t current = __atomic_load_n(slot, __ATOMIC_RELAXED);
  while (value > current &&
         !__atomic_compare_exchange_n(slot, &current, value, true,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
#else
  catomic_max_int32_locked(slot, value);
#endif
}

#endif
