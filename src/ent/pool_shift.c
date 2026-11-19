#include "pool_shift.h"

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../util/fnv.h"
#include "../util/io_util.h"
#include "bag.h"
#include "equity.h"
#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "rack.h"
#include "rack_value_table.h"
#include <math.h>
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
  table->klv = klv;
  table->exact_values = NULL;
  table->has_reference_pool = false;
  table->reference_exact_offset = 0;
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
  const int64_t full_total = table->has_reference_pool ? table->reference_total
                                                       : ld_get_total_tiles(ld);
  // weight * (unseen_t / unseen - dist_t / total), as one fraction rounded
  // to the nearest unit.
  const int64_t denominator = INT64_C(1000) * unseen_total * full_total;
  for (int ml = 0; ml < table->stride; ml++) {
    weights[ml] = 0;
    if (ml >= table->ld_size || denominator == 0) {
      continue;
    }
    const int64_t reference_count = table->has_reference_pool
                                        ? table->reference_counts[ml]
                                        : ld_get_dist(ld, (MachineLetter)ml);
    const int64_t share_difference = (int64_t)unseen_counts[ml] * full_total -
                                     reference_count * unseen_total;
    const int64_t numerator = (int64_t)weight_milli * share_difference *
                              ((int64_t)1 << POOL_SHIFT_WEIGHT_BITS);
    weights[ml] =
        (int32_t)(numerator >= 0
                      ? (numerator + denominator / 2) / denominator
                      : -((denominator / 2 - numerator) / denominator));
  }
}

bool pool_shift_table_set_exact_values(PoolShiftTable *table,
                                       const RackValueTable *rvt,
                                       const LetterDistribution *ld) {
  table->exact_values = NULL;
  if (rvt == NULL || table->ld_size != ld_get_size(ld) ||
      rvt->num_letters != table->ld_size ||
      rack_value_table_get_klv_hash(rvt) != klv_get_hash(table->klv)) {
    return false;
  }
  for (int ml = 0; ml < table->ld_size; ml++) {
    if (rvt->letter_counts[ml] != ld_get_dist(ld, (MachineLetter)ml)) {
      return false;
    }
  }
  table->exact_values = rvt;
  return true;
}

// The unseen tiles of a mover who sees the bag and opponent_rack as unseen.
static int pool_shift_get_unseen_counts(const PoolShiftTable *table,
                                        const Bag *bag,
                                        const Rack *opponent_rack,
                                        int unseen_counts[MAX_ALPHABET_SIZE]) {
  memset(unseen_counts, 0, sizeof(int) * MAX_ALPHABET_SIZE);
  bag_increment_unseen_count(bag, unseen_counts);
  int unseen_total = 0;
  for (int ml = 0; ml < table->ld_size; ml++) {
    unseen_counts[ml] += rack_get_letter(opponent_rack, (MachineLetter)ml);
    unseen_total += unseen_counts[ml];
  }
  return unseen_total;
}

// A walk over the draws that fill a leave to a full rack, tile by tile in
// the order of the letters, which is the order of the RackValueTable's
// racks: only the letters the leave or the unseen tiles hold take part.
typedef struct PoolShiftExactWalk {
  const RackValueTable *rvt;
  int number_of_letters;
  MachineLetter letters[MAX_ALPHABET_SIZE];
  int leave_counts[MAX_ALPHABET_SIZE];
  int unseen_counts[MAX_ALPHABET_SIZE];
  // Of the letters from this one on: the unseen tiles, the leave's tiles,
  // and what the leave's tiles add to a rack's index when no more are drawn.
  int unseen_from[MAX_ALPHABET_SIZE + 1];
  int leave_from[MAX_ALPHABET_SIZE + 1];
  uint32_t leave_index_from[MAX_ALPHABET_SIZE + 1];
  // The number of draws that give a valued rack, and the sum of those racks'
  // values, each rack counted once for every draw that gives it.
  int64_t draws;
  int64_t value_sum;
} PoolShiftExactWalk;

static void pool_shift_exact_walk(PoolShiftExactWalk *walk, int letter_idx,
                                  int tiles_to_draw, uint32_t rack_index,
                                  int64_t ways) {
  if (tiles_to_draw == 0) {
    const int32_t stored = rack_value_table_read_stored(
        walk->rvt, rack_index + walk->leave_index_from[letter_idx]);
    if (stored != RACK_VALUE_TABLE_UNVALUED) {
      walk->draws += ways;
      walk->value_sum += ways * (int64_t)stored;
    }
    return;
  }
  const MachineLetter ml = walk->letters[letter_idx];
  const int unseen = walk->unseen_counts[letter_idx];
  const int leave_count = walk->leave_counts[letter_idx];
  const int tiles = tiles_to_draw + walk->leave_from[letter_idx];
  const int unseen_after = walk->unseen_from[letter_idx + 1];
  const int max_drawn = unseen < tiles_to_draw ? unseen : tiles_to_draw;
  const int min_drawn =
      tiles_to_draw > unseen_after ? tiles_to_draw - unseen_after : 0;
  // C(unseen, drawn), carried from one count to the next.
  int64_t letter_ways = 1;
  for (int drawn = 0; drawn < min_drawn; drawn++) {
    letter_ways = letter_ways * (unseen - drawn) / (drawn + 1);
  }
  for (int drawn = min_drawn; drawn <= max_drawn; drawn++) {
    const int count = leave_count + drawn;
    const uint32_t offset =
        count == 0 ? 0 : walk->rvt->rank_offsets[ml][tiles][count];
    pool_shift_exact_walk(walk, letter_idx + 1, tiles_to_draw - drawn,
                          rack_index + offset, ways * letter_ways);
    letter_ways = letter_ways * (unseen - drawn) / (drawn + 1);
  }
}

// Sets *value to the exact value of leave, of fewer than RACK_SIZE tiles,
// for a mover who sees unseen_counts, and returns true, or returns false if
// no draw gives a rack the table values.
bool pool_shift_get_exact_leave_value(const PoolShiftTable *table,
                                             const int *unseen_counts,
                                             const Rack *leave, Equity *value) {
  PoolShiftExactWalk walk;
  walk.rvt = table->exact_values;
  walk.number_of_letters = 0;
  walk.draws = 0;
  walk.value_sum = 0;
  for (int ml = 0; ml < table->ld_size; ml++) {
    const int leave_count = rack_get_letter(leave, (MachineLetter)ml);
    if (leave_count == 0 && unseen_counts[ml] == 0) {
      continue;
    }
    walk.letters[walk.number_of_letters] = (MachineLetter)ml;
    walk.leave_counts[walk.number_of_letters] = leave_count;
    walk.unseen_counts[walk.number_of_letters] = unseen_counts[ml];
    walk.number_of_letters++;
  }
  walk.unseen_from[walk.number_of_letters] = 0;
  walk.leave_from[walk.number_of_letters] = 0;
  walk.leave_index_from[walk.number_of_letters] = 0;
  for (int letter_idx = walk.number_of_letters - 1; letter_idx >= 0;
       letter_idx--) {
    const int leave_count = walk.leave_counts[letter_idx];
    walk.unseen_from[letter_idx] =
        walk.unseen_from[letter_idx + 1] + walk.unseen_counts[letter_idx];
    walk.leave_from[letter_idx] = walk.leave_from[letter_idx + 1] + leave_count;
    walk.leave_index_from[letter_idx] = walk.leave_index_from[letter_idx + 1];
    if (leave_count != 0) {
      walk.leave_index_from[letter_idx] +=
          walk.rvt->rank_offsets[walk.letters[letter_idx]]
                                [walk.leave_from[letter_idx]][leave_count];
    }
  }
  const int tiles_to_draw = RACK_SIZE - rack_get_total_letters(leave);
  if (tiles_to_draw > walk.unseen_from[0]) {
    return false;
  }
  pool_shift_exact_walk(&walk, 0, tiles_to_draw, 0, 1);
  if (walk.draws == 0) {
    return false;
  }
  const int64_t half = walk.draws / 2;
  *value =
      (Equity)(walk.value_sum >= 0 ? (walk.value_sum + half) / walk.draws
                                   : -((half - walk.value_sum) / walk.draws));
  return true;
}

// The exact value of leave less the KLV's: 0 for a full rack, and where no
// draw gives a rack the table values.
static Equity pool_shift_get_exact_shift(const PoolShiftTable *table,
                                         const int *unseen_counts,
                                         const Rack *leave) {
  Equity exact_value = 0;
  if (rack_get_total_letters(leave) >= RACK_SIZE ||
      !pool_shift_get_exact_leave_value(table, unseen_counts, leave,
                                        &exact_value)) {
    return 0;
  }
  return exact_value - klv_get_leave_value(table->klv, leave);
}

// Whether the table shifts leaves of RACK_SIZE - 1 tiles (see
// reference_exact_offset).
static bool pool_shift_table_shifts_long_leaves(const PoolShiftTable *table,
                                                int weight_milli) {
  // LOCAL MEASUREMENT SWITCH, not for commit: a weight ending in 9
  // thousandths does without, to compare the two in one run.
  return table->exact_values != NULL && weight_milli % 10 != 9;
}

// The shift of a leave of RACK_SIZE - 1 tiles, for a table that shifts them:
// 0 where no draw gives a rack the table values.
static Equity pool_shift_get_long_leave_shift(const PoolShiftTable *table,
                                              const int *unseen_counts,
                                              const Rack *leave) {
  Equity exact_value = 0;
  if (!pool_shift_get_exact_leave_value(table, unseen_counts, leave,
                                        &exact_value)) {
    return 0;
  }
  return exact_value - table->reference_exact_offset -
         klv_get_leave_value(table->klv, leave);
}

// The leaves of one pool (see PoolLeaves). One walk over the full racks
// gives every leave its exact value: each rack is split, letter by letter,
// into the tiles of a leave and the tiles drawn, in every way the pool can
// supply the drawn ones, and its value goes to that leave's sum with the
// number of draws that give the split.
typedef struct PoolLeavesWalk {
  const RackValueTable *rvt;
  int ld_size;
  int unseen_counts[MAX_ALPHABET_SIZE];
  int dist_counts[MAX_ALPHABET_SIZE];
  // ways[ml][drawn] is C(unseen_counts[ml], drawn).
  double ways[MAX_ALPHABET_SIZE][RACK_SIZE + 1];
  // A leave's index among the multisets of up to RACK_SIZE tiles: the sum
  // over its letters of leave_offsets[ml][capacity][count], where capacity
  // is RACK_SIZE less the tiles of the letters before ml. The empty leave's
  // is 0.
  uint32_t leave_offsets[MAX_ALPHABET_SIZE][RACK_SIZE + 1][RACK_SIZE + 1];
  uint32_t number_of_leaves;
  // By leave index: the draws that give a valued rack, and the sum of those
  // racks' values.
  double *draws;
  double *value_sums;
} PoolLeavesWalk;

// The number of multisets of size tiles over symbols symbols.
static uint64_t pool_leaves_count_multisets(int tiles, int symbols) {
  if (symbols == 0) {
    return tiles == 0 ? 1 : 0;
  }
  uint64_t count = 1;
  for (int idx = 1; idx <= tiles; idx++) {
    count = count * (uint64_t)(symbols - 1 + idx) / (uint64_t)idx;
  }
  return count;
}

static void pool_leaves_walk_set_offsets(PoolLeavesWalk *walk) {
  for (int ml = 0; ml < walk->ld_size; ml++) {
    // The letters after ml, and the tiles a leave leaves out.
    const int symbols_after = walk->ld_size - ml;
    for (int capacity = 0; capacity <= RACK_SIZE; capacity++) {
      uint64_t offset = 0;
      for (int count = 0; count <= RACK_SIZE; count++) {
        walk->leave_offsets[ml][capacity][count] = (uint32_t)offset;
        if (count <= capacity) {
          offset +=
              pool_leaves_count_multisets(capacity - count, symbols_after);
        }
      }
    }
  }
  walk->number_of_leaves =
      (uint32_t)pool_leaves_count_multisets(RACK_SIZE, walk->ld_size + 1);
}

static void pool_leaves_walk_racks(PoolLeavesWalk *walk, int ml, int tiles,
                                   uint32_t rack_index, int capacity,
                                   uint32_t leave_index, double ways) {
  if (tiles == 0) {
    const int32_t stored = rack_value_table_read_stored(walk->rvt, rack_index);
    if (stored != RACK_VALUE_TABLE_UNVALUED) {
      walk->draws[leave_index] += ways;
      walk->value_sums[leave_index] += ways * (double)stored;
    }
    return;
  }
  if (ml == walk->ld_size) {
    return;
  }
  const int unseen = walk->unseen_counts[ml];
  const int max_count =
      tiles < walk->dist_counts[ml] ? tiles : walk->dist_counts[ml];
  for (int count = 0; count <= max_count; count++) {
    uint32_t rack_offset = 0;
    if (count != 0) {
      rack_offset = walk->rvt->rank_offsets[ml][tiles][count];
      if (rack_offset == UINT32_MAX) {
        continue;
      }
    }
    const int min_kept = count > unseen ? count - unseen : 0;
    for (int kept = min_kept; kept <= count; kept++) {
      pool_leaves_walk_racks(walk, ml + 1, tiles - count,
                             rack_index + rack_offset, capacity - kept,
                             leave_index +
                                 walk->leave_offsets[ml][capacity][kept],
                             ways * walk->ways[ml][count - kept]);
    }
  }
}

// Sets the pool KLV's value of every leave the letter distribution can deal,
// of up to RACK_SIZE - 1 tiles: its exact value less the empty leave's, or
// the stock KLV's where no draw gives it a valued rack.
static void pool_leaves_fill_klv(const PoolLeavesWalk *walk, const KLV *stock,
                                 KLV *klv, Rack *leave, int ml, int capacity,
                                 uint32_t leave_index, double empty_value) {
  if (ml == walk->ld_size || capacity == 1) {
    return;
  }
  const int max_count = walk->dist_counts[ml] < capacity - 1
                            ? walk->dist_counts[ml]
                            : capacity - 1;
  for (int count = 0; count <= max_count; count++) {
    const uint32_t next_index =
        leave_index + walk->leave_offsets[ml][capacity][count];
    if (count != 0) {
      rack_add_letter(leave, (MachineLetter)ml);
      const uint32_t klv_index = klv_get_word_index(klv, leave);
      if (klv_index != KLV_UNFOUND_INDEX) {
        Equity value = klv_get_indexed_leave_value(stock, klv_index);
        if (walk->draws[next_index] > 0) {
          value = (Equity)llround(walk->value_sums[next_index] /
                                      walk->draws[next_index] -
                                  empty_value);
        }
        klv->leave_values[klv_index] = value;
      }
    }
    pool_leaves_fill_klv(walk, stock, klv, leave, ml + 1, capacity - count,
                         next_index, empty_value);
  }
  for (int count = 0; count < max_count; count++) {
    rack_take_letter(leave, (MachineLetter)ml);
  }
}

PoolLeaves *pool_leaves_create(const PoolShiftTable *table,
                               const LetterDistribution *ld, const Bag *bag,
                               const Rack *opponent_rack,
                               const Rack *mover_rack) {
  const KLV *stock = table->klv;
  PoolLeavesWalk *walk = malloc_or_die(sizeof(PoolLeavesWalk));
  walk->rvt = table->exact_values;
  walk->ld_size = table->ld_size;
  int unseen_total = pool_shift_get_unseen_counts(table, bag, opponent_rack,
                                                  walk->unseen_counts);
  if (mover_rack != NULL) {
    for (int ml = 0; ml < walk->ld_size; ml++) {
      const int held = rack_get_letter(mover_rack, (MachineLetter)ml);
      walk->unseen_counts[ml] += held;
      unseen_total += held;
    }
  }
  for (int ml = 0; ml < walk->ld_size; ml++) {
    walk->dist_counts[ml] = ld_get_dist(ld, (MachineLetter)ml);
    double ways = 1.0;
    for (int drawn = 0; drawn <= RACK_SIZE; drawn++) {
      walk->ways[ml][drawn] = drawn <= walk->unseen_counts[ml] ? ways : 0.0;
      ways = ways * (double)(walk->unseen_counts[ml] - drawn) /
             (double)(drawn + 1);
    }
  }
  pool_leaves_walk_set_offsets(walk);
  walk->draws = calloc_or_die(walk->number_of_leaves, sizeof(double));
  walk->value_sums = calloc_or_die(walk->number_of_leaves, sizeof(double));
  pool_leaves_walk_racks(walk, 0, RACK_SIZE, 0, RACK_SIZE, 0, 1.0);

  const KWG *stock_kwg = klv_get_kwg(stock);
  KWG *kwg = kwg_create_empty();
  const int number_of_nodes = kwg_get_number_of_nodes(stock_kwg);
  kwg_allocate_nodes(kwg, (size_t)number_of_nodes);
  memcpy(kwg_get_mutable_nodes(kwg), kwg_get_mutable_nodes(stock_kwg),
         sizeof(uint32_t) * (size_t)number_of_nodes);
  PoolLeaves *pool_leaves = malloc_or_die(sizeof(PoolLeaves));
  pool_leaves->klv = klv_create_zeroed_from_kwg(
      kwg, (int)klv_get_number_of_leaves(stock), klv_get_name(stock));
  memcpy(pool_leaves->klv->leave_values, stock->leave_values,
         sizeof(Equity) * (size_t)klv_get_number_of_leaves(stock));
  const double empty_value =
      walk->draws[0] > 0 ? walk->value_sums[0] / walk->draws[0] : 0.0;
  Rack leave;
  rack_set_dist_size_and_reset(&leave, walk->ld_size);
  pool_leaves_fill_klv(walk, stock, pool_leaves->klv, &leave, 0, RACK_SIZE, 0,
                       empty_value);
  klv_mark_leave_values_mutated(pool_leaves->klv);
  pool_leaves->table = pool_shift_table_create(pool_leaves->klv, ld);
  pool_leaves->table->has_reference_pool = true;
  pool_leaves->table->exact_values = table->exact_values;
  pool_leaves->table->reference_exact_offset = (Equity)llround(empty_value);
  pool_leaves->table->reference_total = unseen_total;
  memcpy(pool_leaves->table->reference_counts, walk->unseen_counts,
         sizeof(walk->unseen_counts));
  free(walk->value_sums);
  free(walk->draws);
  free(walk);
  return pool_leaves;
}

void pool_leaves_destroy(PoolLeaves *pool_leaves) {
  if (pool_leaves == NULL) {
    return;
  }
  pool_shift_table_destroy(pool_leaves->table);
  klv_destroy(pool_leaves->klv);
  free(pool_leaves);
}

void pool_shift_fill_rack(const PoolShiftTable *table,
                          const LetterDistribution *ld, const Bag *bag,
                          const Rack *opponent_rack, int weight_milli,
                          const Rack *rack, PoolShiftRack *result) {
  const bool is_exact = weight_milli == POOL_SHIFT_EXACT_WEIGHT_MILLI;
  if (!is_exact) {
    pool_shift_fill_weights(table, ld, bag, opponent_rack, weight_milli,
                            result->weights);
  }
  result->ranks[0] = 0;
  result->map_indices[0] = 0;
  result->sizes[0] = 0;
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
      }
      count += block_size;
    }
  }
  result->count = count;
  if (is_exact) {
    int unseen_counts[MAX_ALPHABET_SIZE];
    pool_shift_get_unseen_counts(table, bag, opponent_rack, unseen_counts);
    // What is remembered by a rack's weights is remembered by its pool
    // (see PoolShiftRack): here they are the unseen tiles themselves.
    for (int ml = 0; ml < POOL_SHIFT_MAX_STRIDE; ml++) {
      result->weights[ml] = ml < table->ld_size ? unseen_counts[ml] : 0;
    }
    Rack leave;
    for (int idx = 0; idx < count; idx++) {
      rack_set_dist_size_and_reset(&leave, table->ld_size);
      for (int bit_idx = 0; bit_idx < RACK_SIZE; bit_idx++) {
        if ((result->map_indices[idx] & (1 << bit_idx)) != 0) {
          rack_add_letter(&leave, result->bit_tiles[bit_idx]);
        }
      }
      result->shifts[idx] =
          pool_shift_get_exact_shift(table, unseen_counts, &leave);
    }
    return;
  }
  for (int idx = 0; idx < count; idx++) {
    if (result->sizes[idx] <= POOL_SHIFT_MAX_LEAVE_SIZE) {
      pool_shift_table_prefetch_row(table, result->ranks[idx]);
    }
  }
  const bool shifts_long_leaves =
      pool_shift_table_shifts_long_leaves(table, weight_milli);
  int unseen_counts[MAX_ALPHABET_SIZE];
  if (shifts_long_leaves) {
    pool_shift_get_unseen_counts(table, bag, opponent_rack, unseen_counts);
  }
  for (int idx = 0; idx < count; idx++) {
    const int size = result->sizes[idx];
    if (size <= POOL_SHIFT_MAX_LEAVE_SIZE) {
      result->shifts[idx] = pool_shift_table_get_shift(
          table, result->weights, result->ranks[idx], size);
      continue;
    }
    result->shifts[idx] = 0;
    if (size != RACK_SIZE - 1 || !shifts_long_leaves) {
      continue;
    }
    Rack leave;
    rack_set_dist_size_and_reset(&leave, table->ld_size);
    for (int bit_idx = 0; bit_idx < RACK_SIZE; bit_idx++) {
      if ((result->map_indices[idx] & (1 << bit_idx)) != 0) {
        rack_add_letter(&leave, result->bit_tiles[bit_idx]);
      }
    }
    result->shifts[idx] =
        pool_shift_get_long_leave_shift(table, unseen_counts, &leave);
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
  if (weight_milli == POOL_SHIFT_EXACT_WEIGHT_MILLI) {
    int unseen_counts[MAX_ALPHABET_SIZE];
    pool_shift_get_unseen_counts(table, bag, opponent_rack, unseen_counts);
    return pool_shift_get_exact_shift(table, unseen_counts, leave);
  }
  const uint32_t row = pool_shift_table_get_leave_row(table, leave);
  if (row == POOL_SHIFT_NO_ROW) {
    if (rack_get_total_letters(leave) != RACK_SIZE - 1 ||
        !pool_shift_table_shifts_long_leaves(table, weight_milli)) {
      return 0;
    }
    int unseen_counts[MAX_ALPHABET_SIZE];
    pool_shift_get_unseen_counts(table, bag, opponent_rack, unseen_counts);
    return pool_shift_get_long_leave_shift(table, unseen_counts, leave);
  }
  int32_t weights[POOL_SHIFT_MAX_STRIDE];
  pool_shift_fill_weights(table, ld, bag, opponent_rack, weight_milli, weights);
  return pool_shift_table_get_shift(table, weights, row,
                                    rack_get_total_letters(leave));
}
