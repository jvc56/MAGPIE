#include "catomic.h"

#include <stdatomic.h>
#include <stdint.h>

enum {
  // Spinlocks shared by slots whose addresses hash alike, a power of 2.
  CATOMIC_LOCK_STRIPES = 256,
};

// Zero, unlocked, from the static zero initialization, which is a valid state
// for atomic objects of static storage duration.
static _Atomic int catomic_locks[CATOMIC_LOCK_STRIPES];

void catomic_max_int32_locked(int32_t *slot, int32_t value) {
  _Atomic int *lock =
      &catomic_locks[((uintptr_t)slot / sizeof(int32_t)) &
                     (CATOMIC_LOCK_STRIPES - 1)];
  while (atomic_exchange_explicit(lock, 1, memory_order_acquire) != 0) {
  }
  if (value > *slot) {
    *slot = value;
  }
  atomic_store_explicit(lock, 0, memory_order_release);
}
