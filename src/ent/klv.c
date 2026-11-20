#include "klv.h"

#include <stdatomic.h>
#include <stdint.h>

// Counted across every thread that loads or builds a KLV.
static atomic_uint_fast64_t klvs_created = 0;

uint64_t klv_next_instance_id(void) {
  return (uint64_t)atomic_fetch_add(&klvs_created, 1) + 1;
}
