#ifndef PEG_MEMORY_H
#define PEG_MEMORY_H

#include "../compat/memory_info.h"
#include "../ent/transposition_table.h"
#include "../ent/zobrist.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Maximum table count that fits even at the minimum entry count.
static inline int peg_memory_max_tables(double total_fraction) {
  const uint64_t minimum =
      ((uint64_t)TTENTRY_SIZE_BYTES << PEG_TT_MIN_SIZE_POWER) +
      (NPROC_SIZE * sizeof(atomic_uchar)) + sizeof(TranspositionTable) +
      zobrist_allocation_size();
  return (int)(total_fraction * (double)get_search_memory_budget_base() /
               (double)minimum);
}

// Reserve overhead as well as entries within an aggregate worker budget.
static inline bool peg_memory_worker_fraction(double total_fraction,
                                              int workers,
                                              double *worker_fraction) {
  if (workers < 1) {
    return false;
  }
  const double memory = (double)get_search_memory_budget_base();
  const uint64_t overhead = (NPROC_SIZE * sizeof(atomic_uchar)) +
                            sizeof(TranspositionTable) +
                            zobrist_allocation_size();
  const double fraction =
      (total_fraction / workers) - ((double)overhead / memory);
  const double minimum =
      (double)((uint64_t)TTENTRY_SIZE_BYTES << PEG_TT_MIN_SIZE_POWER) / memory;
  if (fraction < minimum) {
    return false;
  }
  *worker_fraction = fraction;
  return true;
}

#endif
