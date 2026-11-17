#include "pool_shift.h"

#include "../def/letter_distribution_defs.h"
#include "../util/fnv.h"
#include "../util/io_util.h"
#include "bag.h"
#include "equity.h"
#include "klv.h"
#include "letter_distribution.h"
#include "rack.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

// Fills the rows of the build's leave, which is numbered rank among the
// leaves of its size, and of every leave of the distribution that adds tiles
// from min_ml up to it.
static void pool_shift_build_walk(PoolShiftBuild *build, uint32_t rank,
                                  int min_ml) {
  const PoolShiftTable *table = build->table;
  const int leave_size = rack_get_total_letters(&build->leave);
  pool_shift_build_fill_row(build,
                            pool_shift_table_get_row(table, rank, leave_size));
  if (leave_size == POOL_SHIFT_MAX_LEAVE_SIZE) {
    return;
  }
  for (int ml = min_ml; ml < table->ld_size; ml++) {
    if (rack_get_letter(&build->leave, (MachineLetter)ml) >=
        ld_get_dist(build->ld, (MachineLetter)ml)) {
      continue;
    }
    const uint32_t extended_rank = pool_shift_table_extend_rank(
        table, rank, leave_size, (MachineLetter)ml);
    rack_add_letter(&build->leave, (MachineLetter)ml);
    pool_shift_build_walk(build, extended_rank, ml);
    rack_take_letter(&build->leave, (MachineLetter)ml);
  }
}

// Sets rank_terms, size_starts and number_of_rows for the table's ld_size.
static void pool_shift_table_set_numbering(PoolShiftTable *table) {
  // C(t + i, i + 1) from C(t + i - 1, i), the row before, by
  // C(n, k) = C(n - 1, k - 1) * n / k, which divides exactly.
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    uint64_t term = (uint64_t)ml;
    for (int tiles_before = 0; tiles_before < POOL_SHIFT_MAX_LEAVE_SIZE;
         tiles_before++) {
      if (tiles_before > 0) {
        term =
            term * (uint64_t)(ml + tiles_before) / (uint64_t)(tiles_before + 1);
      }
      table->rank_terms[tiles_before][ml] = (uint32_t)term;
    }
  }
  // There are C(ld_size + k - 1, k) multisets of k tiles, which is the
  // number of the first one whose greatest tile would be ld_size.
  uint32_t number_of_rows = 0;
  for (int leave_size = 0; leave_size <= POOL_SHIFT_MAX_LEAVE_SIZE;
       leave_size++) {
    table->size_starts[leave_size] = number_of_rows;
    uint64_t size_count = 1;
    for (int tile_idx = 0; tile_idx < leave_size; tile_idx++) {
      size_count = size_count * (uint64_t)(table->ld_size + tile_idx) /
                   (uint64_t)(tile_idx + 1);
    }
    number_of_rows += (uint32_t)size_count;
  }
  table->number_of_rows = number_of_rows;
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
  pool_shift_build_walk(&build, 0, 0);
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
  for (int ml = 0; ml < table->ld_size; ml++) {
    const int num_this = rack_get_letter(leave, (MachineLetter)ml);
    for (int copy_idx = 0; copy_idx < num_this; copy_idx++) {
      rank = pool_shift_table_extend_rank(table, rank, tiles_before,
                                          (MachineLetter)ml);
      tiles_before++;
    }
  }
  return pool_shift_table_get_row(table, rank, leave_size);
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
                      ? (numerator + denominator / 2) / denominator
                      : -((denominator / 2 - numerator) / denominator));
  }
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
