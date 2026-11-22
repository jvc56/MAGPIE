#include "pool_shift.h"

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../util/fnv.h"
#include "../util/io_util.h"
#include "bag.h"
#include "equity.h"
#include "klv.h"
#include "letter_distribution.h"
#include "rack.h"
#include <float.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Counted across every thread that builds a table.
static atomic_uint_fast64_t pool_shift_tables_created = 0;

static uint64_t pool_shift_get_ld_hash(const LetterDistribution *ld) {
  uint64_t hash = FNV_64_OFFSET_BASIS;
  hash = fnv64a_step(hash, (uint64_t)ld_get_size(ld));
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    hash = fnv64a_step(hash, (uint64_t)ld_get_dist(ld, (MachineLetter)ml));
  }
  return hash;
}

bool pool_shift_table_matches_ld(const PoolShiftTable *table,
                                 const LetterDistribution *ld) {
  return table->ld_size == ld_get_size(ld) &&
         table->ld_hash == pool_shift_get_ld_hash(ld);
}

typedef struct PoolShiftBuild {
  const KLV *klv;
  const LetterDistribution *ld;
  PoolShiftTable *table;
  Rack leave;
} PoolShiftBuild;

// Fills the row of the build's leave: the value of the leave with one more
// of each tile it does not hold every copy of.
static void pool_shift_build_fill_row(PoolShiftBuild *build, uint32_t row) {
  const PoolShiftTable *table = build->table;
  Equity *values = table->values + (size_t)row * (size_t)table->stride;
  for (int ml = 0; ml < table->ld_size; ml++) {
    if (rack_get_letter(&build->leave, (MachineLetter)ml) >=
        ld_get_dist(build->ld, (MachineLetter)ml)) {
      continue;
    }
    rack_add_letter(&build->leave, (MachineLetter)ml);
    values[ml] = klv_get_leave_value(build->klv, &build->leave);
    rack_take_letter(&build->leave, (MachineLetter)ml);
  }
}

// Fills the row at rank and the rows of leaves formed by adding tiles
// at or below max_ml.
static void pool_shift_build_walk(PoolShiftBuild *build, uint32_t rank,
                                  int max_ml) {
  const PoolShiftTable *table = build->table;
  const int leave_size = rack_get_total_letters(&build->leave);
  pool_shift_build_fill_row(build, rank);
  if (leave_size == POOL_SHIFT_MAX_LEAVE_SIZE) {
    return;
  }
  for (int ml = max_ml; ml >= 0; ml--) {
    if (rack_get_letter(&build->leave, (MachineLetter)ml) >=
        ld_get_dist(build->ld, (MachineLetter)ml)) {
      continue;
    }
    const uint32_t extended_rank =
        rank + table->rank_increments[leave_size][ml];
    rack_add_letter(&build->leave, (MachineLetter)ml);
    pool_shift_build_walk(build, extended_rank, ml);
    rack_take_letter(&build->leave, (MachineLetter)ml);
  }
}

// The binomial term and the distance to the next size block depend only on
// the alphabet and the old size. Folding them together also gives ranks of
// the larger subsets used as played tiles in a census lookup.
static void pool_shift_table_set_numbering(PoolShiftTable *table) {
  uint64_t size_counts[RACK_SIZE];
  size_counts[0] = 1;
  table->number_of_rows = 0;
  for (int size = 0; size < RACK_SIZE; size++) {
    if (size > 0) {
      size_counts[size] = size_counts[size - 1] *
                          (uint64_t)(table->ld_size + size - 1) /
                          (uint64_t)size;
    }
    if (size <= POOL_SHIFT_MAX_LEAVE_SIZE) {
      table->number_of_rows += (uint32_t)size_counts[size];
    }
  }
  for (int ml = 0; ml < table->ld_size; ml++) {
    const int reverse_ml = table->ld_size - 1 - ml;
    uint64_t term = (uint64_t)reverse_ml;
    for (int size = 0; size < RACK_SIZE; size++) {
      if (size > 0) {
        term = term * (uint64_t)(reverse_ml + size) / (uint64_t)(size + 1);
      }
      table->rank_increments[size][ml] = (uint32_t)(size_counts[size] + term);
    }
  }
}

PoolShiftTable *pool_shift_table_create(const KLV *klv,
                                        const LetterDistribution *ld) {
  PoolShiftTable *table = malloc_or_die(sizeof(PoolShiftTable));
  table->instance_id =
      (uint64_t)atomic_fetch_add(&pool_shift_tables_created, 1) + 1;
  table->klv_instance_id = klv_get_instance_id(klv);
  table->klv_mutation_counter = klv_get_mutation_counter(klv);
  table->ld_hash = pool_shift_get_ld_hash(ld);
  table->ld_size = ld_get_size(ld);
  table->stride = (table->ld_size + 3) & ~3;
  pool_shift_table_set_numbering(table);
  table->values = calloc_or_die(
      (size_t)table->number_of_rows * (size_t)table->stride, sizeof(Equity));
  PoolShiftBuild build = {.klv = klv, .ld = ld, .table = table};
  rack_set_dist_size_and_reset(&build.leave, table->ld_size);
  pool_shift_build_walk(&build, 0, table->ld_size - 1);
  return table;
}

void pool_shift_table_destroy(PoolShiftTable *table) {
  if (!table) {
    return;
  }
  free(table->values);
  free(table);
}

uint32_t pool_shift_table_get_leave_row(const PoolShiftTable *table,
                                        const Rack *leave) {
  const int leave_size = rack_get_total_letters(leave);
  if (leave_size > POOL_SHIFT_MAX_LEAVE_SIZE ||
      rack_get_dist_size(leave) != table->ld_size) {
    return POOL_SHIFT_NO_ROW;
  }
  uint32_t rank = 0;
  int tiles_before = 0;
  for (int ml = table->ld_size - 1; ml >= 0; ml--) {
    const int num_this = rack_get_letter(leave, (MachineLetter)ml);
    for (int copy_idx = 0; copy_idx < num_this; copy_idx++) {
      rank += table->rank_increments[tiles_before][ml];
      tiles_before++;
    }
  }
  return rank;
}

// dividend / divisor as integer division gives it, for a dividend of 0 or
// more and a divisor above 0. Both convert to doubles exactly while they are
// below 2^DBL_MANT_DIG, and the doubles' quotient, cut to an integer, is then
// the integers' quotient or one more. One multiplication tells which, and all
// of it costs less than a 64-bit integer division where that is slow.
static inline int64_t pool_shift_divide(int64_t dividend, int64_t divisor) {
  const int64_t exact_double_limit = (int64_t)1 << DBL_MANT_DIG;
  if (dividend >= exact_double_limit || divisor >= exact_double_limit) {
    return dividend / divisor;
  }
  const int64_t quotient = (int64_t)((double)dividend / (double)divisor);
  return quotient * divisor > dividend ? quotient - 1 : quotient;
}

void pool_shift_fill_weights(const PoolShiftTable *table,
                             const LetterDistribution *ld, const Bag *bag,
                             const Rack *opponent_rack, int weight_milli,
                             int32_t weights[POOL_SHIFT_MAX_STRIDE]) {
  int unseen_counts[MAX_ALPHABET_SIZE] = {0};
  bag_increment_unseen_count(bag, unseen_counts);
  int64_t unseen_total = 0;
  for (int ml = 0; ml < table->ld_size; ml++) {
    unseen_counts[ml] += rack_get_letter(opponent_rack, (MachineLetter)ml);
    unseen_total += unseen_counts[ml];
  }
  const int64_t full_total = ld_get_total_tiles(ld);
  // weight * (unseen_t / unseen - dist_t / total), as one fraction rounded
  // to the nearest unit.
  const int64_t denominator = INT64_C(1000) * unseen_total * full_total;
  for (int ml = 0; ml < table->stride; ml++) {
    weights[ml] = 0;
    if (ml >= table->ld_size || denominator == 0) {
      continue;
    }
    const int64_t share_difference =
        (int64_t)unseen_counts[ml] * full_total -
        (int64_t)ld_get_dist(ld, (MachineLetter)ml) * unseen_total;
    const int64_t numerator = (int64_t)weight_milli * share_difference *
                              ((int64_t)1 << POOL_SHIFT_WEIGHT_BITS);
    weights[ml] =
        (int32_t)(numerator >= 0
                      ? pool_shift_divide(numerator + denominator / 2,
                                          denominator)
                      : -pool_shift_divide(denominator / 2 - numerator,
                                           denominator));
  }
}

// Sets result->values (see PoolShiftRack): value_sources has where the
// table holds each subset's own value, in the row of the subset it extends,
// at the tile added. The full rack, last, extends a subset too large to
// have a row.
static void pool_shift_fill_rack_values(const PoolShiftTable *table,
                                        const size_t *value_sources,
                                        PoolShiftRack *result) {
  result->values[0] = 0;
  for (int idx = 1; idx < result->count - 1; idx++) {
    result->values[idx] = table->values[value_sources[idx]];
  }
  result->values[result->count - 1] = 0;
}

void pool_shift_fill_rack(const PoolShiftTable *table,
                          const LetterDistribution *ld, const Bag *bag,
                          const Rack *opponent_rack, int weight_milli,
                          const Rack *rack, PoolShiftRack *result) {
  pool_shift_fill_weights(table, ld, bag, opponent_rack, weight_milli,
                          result->weights);
  result->ranks[0] = 0;
  result->map_indices[0] = 0;
  result->sizes[0] = 0;
  size_t value_sources[1 << RACK_SIZE];
  // The empty leave extends nothing: its place is never read.
  value_sources[0] = 0;
  int count = 1;
  int base_bit = RACK_SIZE;
  for (int ml = table->ld_size - 1; ml >= 0; ml--) {
    const int copies = rack_get_letter(rack, (MachineLetter)ml);
    const int block_size = count;
    base_bit -= copies;
    for (int copy = 0; copy < copies; copy++) {
      result->bit_tiles[base_bit + copy] = (MachineLetter)ml;
      const int bit = 1 << (base_bit + copy);
      // Extend only the preceding block. For repeated letters this adds a
      // copy only to leaves that already kept every preceding copy.
      for (int src = count - block_size; src < count; src++) {
        const int dst = src + block_size;
        const int size = result->sizes[src];
        result->sizes[dst] = (uint8_t)(size + 1);
        result->map_indices[dst] = result->map_indices[src] | bit;
        result->ranks[dst] =
            result->ranks[src] + table->rank_increments[size][ml];
        value_sources[dst] =
            (size_t)result->ranks[src] * (size_t)table->stride + (size_t)ml;
      }
      count += block_size;
    }
  }
  result->count = count;
  for (int idx = 0; idx < count; idx++) {
    if (result->sizes[idx] <= POOL_SHIFT_MAX_LEAVE_SIZE) {
      pool_shift_table_prefetch_row(table, result->ranks[idx]);
    }
  }
  for (int idx = 0; idx < count; idx++) {
    const int size = result->sizes[idx];
    result->shifts[idx] =
        size > POOL_SHIFT_MAX_LEAVE_SIZE
            ? 0
            : pool_shift_table_get_shift(table, result->weights,
                                         result->ranks[idx], size);
  }
  // Every row read here was read for a shift above.
  pool_shift_fill_rack_values(table, value_sources, result);
}

Equity pool_shift_get_leave_shift(const PoolShiftTable *table, const KLV *klv,
                                  int weight_milli,
                                  const LetterDistribution *ld, const Bag *bag,
                                  const Rack *opponent_rack, const Rack *leave,
                                  int tiles_on_rack, int real_bag_size) {
  if (!pool_shift_applies(table, klv, weight_milli, tiles_on_rack,
                          real_bag_size) ||
      table->ld_size != ld_get_size(ld)) {
    return 0;
  }
  const uint32_t row = pool_shift_table_get_leave_row(table, leave);
  if (row == POOL_SHIFT_NO_ROW) {
    return 0;
  }
  int32_t weights[POOL_SHIFT_MAX_STRIDE];
  pool_shift_fill_weights(table, ld, bag, opponent_rack, weight_milli, weights);
  return pool_shift_table_get_shift(table, weights, row,
                                    rack_get_total_letters(leave));
}
