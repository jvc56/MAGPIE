#include "pool_shift.h"

#include "../def/letter_distribution_defs.h"
#include "../util/fnv.h"
#include "../util/io_util.h"
#include "bag.h"
#include "equity.h"
#include "klv.h"
#include "kwg.h"
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

// A walk of the KLV's word graph in its own order, which numbers each leave
// by how many come before it.
typedef struct PoolShiftBuild {
  const KLV *klv;
  const KWG *kwg;
  const LetterDistribution *ld;
  // NULL while the rows are only being counted.
  PoolShiftTable *table;
  Rack leave;
  // The tiles on the walk's path that the leave above does not hold.
  int tiles_outside_distribution;
  uint32_t klv_leave_index;
  uint32_t next_row;
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

// Visits the leaves under the sibling list at node_index, giving a row to
// each of at most POOL_SHIFT_MAX_LEAVE_SIZE tiles of the distribution.
static void pool_shift_build_walk(PoolShiftBuild *build, uint32_t node_index) {
  const int ld_size = ld_get_size(build->ld);
  for (;; node_index++) {
    const uint32_t node = kwg_node(build->kwg, node_index);
    const int ml = (int)kwg_node_tile(node);
    // A leave with a tile the distribution lacks, or too many of one, gets
    // no row, and neither does any leave that extends it. They are still
    // walked, since they count toward the indexes of the leaves after them.
    const bool in_distribution =
        ml < ld_size && rack_get_letter(&build->leave, (MachineLetter)ml) <
                            ld_get_dist(build->ld, (MachineLetter)ml);
    if (in_distribution) {
      rack_add_letter(&build->leave, (MachineLetter)ml);
    } else {
      build->tiles_outside_distribution++;
    }
    if (kwg_node_accepts(node)) {
      if (build->tiles_outside_distribution == 0 &&
          rack_get_total_letters(&build->leave) <= POOL_SHIFT_MAX_LEAVE_SIZE) {
        if (build->table != NULL) {
          build->table->rows[build->klv_leave_index] = build->next_row;
          pool_shift_build_fill_row(build, build->next_row);
        }
        build->next_row++;
      }
      build->klv_leave_index++;
    }
    const uint32_t arc_index = kwg_node_arc_index(node);
    if (arc_index != 0) {
      pool_shift_build_walk(build, arc_index);
    }
    if (in_distribution) {
      rack_take_letter(&build->leave, (MachineLetter)ml);
    } else {
      build->tiles_outside_distribution--;
    }
    if (kwg_node_is_end(node)) {
      return;
    }
  }
}

// One pass over the KLV: counts the rows, and fills them when the build has
// a table. Returns the number of rows, the empty leave's included.
static uint32_t pool_shift_build_run(PoolShiftBuild *build) {
  rack_set_dist_size_and_reset(&build->leave, ld_get_size(build->ld));
  build->tiles_outside_distribution = 0;
  build->klv_leave_index = 0;
  build->next_row = POOL_SHIFT_EMPTY_LEAVE_ROW;
  if (build->table != NULL) {
    pool_shift_build_fill_row(build, build->next_row);
  }
  build->next_row++;
  const uint32_t root_node_index = kwg_get_dawg_root_node_index(build->kwg);
  if (root_node_index != 0) {
    pool_shift_build_walk(build, root_node_index);
  }
  return build->next_row;
}

PoolShiftTable *pool_shift_table_create(const KLV *klv,
                                        const LetterDistribution *ld) {
  PoolShiftBuild build = {
      .klv = klv, .kwg = klv_get_kwg(klv), .ld = ld, .table = NULL};
  const uint32_t number_of_rows = pool_shift_build_run(&build);
  PoolShiftTable *table = malloc_or_die(sizeof(PoolShiftTable));
  table->instance_id =
      (uint64_t)atomic_fetch_add(&pool_shift_tables_created, 1) + 1;
  table->klv_instance_id = klv_get_instance_id(klv);
  table->klv_mutation_counter = klv_get_mutation_counter(klv);
  table->ld_hash = pool_shift_get_ld_hash(ld);
  table->ld_size = ld_get_size(ld);
  table->stride = (table->ld_size + 3) & ~3;
  table->number_of_leaves = klv_get_number_of_leaves(klv);
  table->number_of_rows = number_of_rows;
  const size_t rows_bytes = sizeof(uint32_t) * (size_t)table->number_of_leaves;
  table->rows = malloc_or_die(rows_bytes > 0 ? rows_bytes : sizeof(uint32_t));
  // Every byte 0xFF is POOL_SHIFT_NO_ROW.
  memset(table->rows, 0xFF, rows_bytes);
  table->values = calloc_or_die((size_t)number_of_rows * (size_t)table->stride,
                                sizeof(Equity));
  build.table = table;
  pool_shift_build_run(&build);
  return table;
}

void pool_shift_table_destroy(PoolShiftTable *table) {
  if (!table) {
    return;
  }
  free(table->rows);
  free(table->values);
  free(table);
}

uint32_t pool_shift_table_get_leave_row(const PoolShiftTable *table,
                                        const KLV *klv, const Rack *leave) {
  const int leave_size = rack_get_total_letters(leave);
  if (leave_size == 0) {
    return POOL_SHIFT_EMPTY_LEAVE_ROW;
  }
  if (leave_size > POOL_SHIFT_MAX_LEAVE_SIZE) {
    return POOL_SHIFT_NO_ROW;
  }
  return pool_shift_table_get_row(table, klv_get_word_index(klv, leave),
                                  leave_size);
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
  const uint32_t row = pool_shift_table_get_leave_row(table, klv, leave);
  if (row == POOL_SHIFT_NO_ROW) {
    return 0;
  }
  int32_t weights[POOL_SHIFT_MAX_STRIDE];
  pool_shift_fill_weights(table, ld, bag, opponent_rack, weight_milli, weights);
  return pool_shift_table_get_shift(table, weights, row,
                                    rack_get_total_letters(leave));
}
