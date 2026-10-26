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

// The fraction for one endgame table shared by `workers` workers, given the
// fraction each would get for a table of its own: the largest power-of-two
// multiple of it that does not exceed `workers`. Scaling a fraction by a power
// of two scales the table's rounded-down power-of-two entry count by that same
// power, so the shared table never holds more entries than the per-worker
// tables would in total, and it pays the per-table overhead once.
static inline double peg_memory_shared_fraction(double worker_fraction,
                                                int workers) {
  int tables = 1;
  while (tables <= workers / 2) {
    tables *= 2;
  }
  return worker_fraction * tables;
}

#endif
