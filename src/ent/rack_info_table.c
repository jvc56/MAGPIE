#include "rack_info_table.h"

#include <stdatomic.h>
#include <stdint.h>

// Counted across every thread that loads or builds a table.
static atomic_uint_fast64_t rack_info_tables_created = 0;

uint64_t rack_info_table_next_instance_id(void) {
  return (uint64_t)atomic_fetch_add(&rack_info_tables_created, 1) + 1;
}
