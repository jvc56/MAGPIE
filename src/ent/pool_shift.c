#include "pool_shift.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
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
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // The exact values of a rack's leaves add each full rack with the others
  // that share all but this many of its last tiles (see PoolShiftRackTail).
  POOL_SHIFT_RACK_TAIL_TILES = (RACK_SIZE) > 3 ? 3 : (RACK_SIZE)-1,
};

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

// The seen-letter mask of pool_shift_klv_walk_siblings.
static_assert(MAX_ALPHABET_SIZE <= 64, "a letter's bit fits a uint64_t");

// The threads of one build: the caller's and its helpers, which run batches
// of units. The units of a batch run once each, in any order, on any of the
// threads: a unit writes nothing another unit of its batch reads or writes.
typedef void (*PoolShiftUnitRun)(void *context, int unit_idx);

typedef struct PoolShiftCrew {
  int number_of_helpers;
  cpthread_t *helpers;
  cpthread_mutex_t mutex;
  cpthread_cond_t batch_posted;
  cpthread_cond_t batch_done;
  // The batch, set under the mutex before its number: a helper reads it
  // after seeing the number, and it stays until every helper is done.
  PoolShiftUnitRun run;
  void *context;
  int number_of_units;
  atomic_int next_unit;
  uint64_t batch_number;
  // The helpers yet to finish the batch.
  int helpers_in_batch;
  bool is_dismissed;
} PoolShiftCrew;

static void pool_shift_crew_run_units(PoolShiftCrew *crew) {
  while (true) {
    const int unit_idx =
        atomic_fetch_add_explicit(&crew->next_unit, 1, memory_order_relaxed);
    if (unit_idx >= crew->number_of_units) {
      return;
    }
    crew->run(crew->context, unit_idx);
  }
}

static void *pool_shift_crew_help(void *uncasted_crew) {
  PoolShiftCrew *crew = uncasted_crew;
  uint64_t batches_seen = 0;
  cpthread_mutex_lock(&crew->mutex);
  while (true) {
    while (!crew->is_dismissed && crew->batch_number == batches_seen) {
      cpthread_cond_wait(&crew->batch_posted, &crew->mutex);
    }
    if (crew->is_dismissed) {
      break;
    }
    batches_seen = crew->batch_number;
    cpthread_mutex_unlock(&crew->mutex);
    pool_shift_crew_run_units(crew);
    cpthread_mutex_lock(&crew->mutex);
    crew->helpers_in_batch--;
    if (crew->helpers_in_batch == 0) {
      cpthread_cond_signal(&crew->batch_done);
    }
  }
  cpthread_mutex_unlock(&crew->mutex);
  return NULL;
}

// A crew of number_of_threads threads, the caller's among them, or NULL for
// one thread.
static PoolShiftCrew *pool_shift_crew_create(int number_of_threads) {
  if (number_of_threads < 2) {
    return NULL;
  }
  PoolShiftCrew *crew = malloc_or_die(sizeof(PoolShiftCrew));
  crew->number_of_helpers = number_of_threads - 1;
  crew->helpers =
      malloc_or_die(sizeof(cpthread_t) * (size_t)crew->number_of_helpers);
  cpthread_mutex_init(&crew->mutex);
  cpthread_cond_init(&crew->batch_posted);
  cpthread_cond_init(&crew->batch_done);
  crew->run = NULL;
  crew->context = NULL;
  crew->number_of_units = 0;
  atomic_init(&crew->next_unit, 0);
  crew->batch_number = 0;
  crew->helpers_in_batch = 0;
  crew->is_dismissed = false;
  for (int helper_idx = 0; helper_idx < crew->number_of_helpers; helper_idx++) {
    cpthread_create(&crew->helpers[helper_idx], pool_shift_crew_help, crew);
  }
  return crew;
}

static void pool_shift_crew_destroy(PoolShiftCrew *crew) {
  if (crew == NULL) {
    return;
  }
  cpthread_mutex_lock(&crew->mutex);
  crew->is_dismissed = true;
  cpthread_cond_broadcast(&crew->batch_posted);
  cpthread_mutex_unlock(&crew->mutex);
  for (int helper_idx = 0; helper_idx < crew->number_of_helpers; helper_idx++) {
    cpthread_join(crew->helpers[helper_idx]);
  }
  free(crew->helpers);
  free(crew);
}

// Runs the units of a batch and returns when all are done. With a NULL crew
// they run on the caller's thread, in order.
static void pool_shift_crew_run(PoolShiftCrew *crew, PoolShiftUnitRun run,
                                void *context, int number_of_units) {
  if (crew == NULL || number_of_units < 2) {
    for (int unit_idx = 0; unit_idx < number_of_units; unit_idx++) {
      run(context, unit_idx);
    }
    return;
  }
  cpthread_mutex_lock(&crew->mutex);
  crew->run = run;
  crew->context = context;
  crew->number_of_units = number_of_units;
  atomic_store(&crew->next_unit, 0);
  crew->helpers_in_batch = crew->number_of_helpers;
  crew->batch_number++;
  cpthread_cond_broadcast(&crew->batch_posted);
  cpthread_mutex_unlock(&crew->mutex);
  pool_shift_crew_run_units(crew);
  cpthread_mutex_lock(&crew->mutex);
  while (crew->helpers_in_batch > 0) {
    cpthread_cond_wait(&crew->batch_done, &crew->mutex);
  }
  cpthread_mutex_unlock(&crew->mutex);
}

// Called for a leave, its letters in ascending order, at the index
// klv_get_word_index finds it at.
typedef void (*PoolShiftLeaveVisit)(void *context, const MachineLetter *letters,
                                    int number_of_letters, uint32_t klv_index);

// A depth-first walk of a KLV's word graph that visits every leave of 1 to
// RACK_SIZE - 1 tiles the letter distribution can deal and the KLV has, at
// the index klv_get_word_index finds it at. It does the lookup's arithmetic
// (increment_node_to_ml, follow_arc): a sibling passed over adds its word
// count, an arc followed adds 1. Only paths of non-decreasing letters are
// lookups, and of siblings of one letter only the first. max_counts holds
// the most of each letter a leave visited has: the letter distribution's
// count, or 0 for a letter whose leaves are left out.
typedef struct PoolShiftKlvWalk {
  const KLV *klv;
  int ld_size;
  int max_counts[MAX_ALPHABET_SIZE];
  int leave_counts[MAX_ALPHABET_SIZE];
  MachineLetter letters[RACK_SIZE];
  PoolShiftLeaveVisit visit;
  void *context;
} PoolShiftKlvWalk;

static void pool_shift_klv_walk_siblings(PoolShiftKlvWalk *walk,
                                         uint32_t node_index,
                                         uint32_t word_index,
                                         int number_of_letters) {
  const KLV *klv = walk->klv;
  const KWG *kwg = klv_get_kwg(klv);
  const uint32_t min_ml =
      number_of_letters == 0 ? 0 : walk->letters[number_of_letters - 1];
  uint64_t seen_letters = 0;
  for (;; node_index++) {
    const uint32_t node = kwg_node(kwg, node_index);
    const uint32_t ml = kwg_node_tile(node);
    if (ml < (uint32_t)walk->ld_size &&
        (seen_letters & ((uint64_t)1 << ml)) == 0) {
      seen_letters |= (uint64_t)1 << ml;
      if (ml >= min_ml && walk->leave_counts[ml] < walk->max_counts[ml]) {
        walk->letters[number_of_letters] = (MachineLetter)ml;
        walk->leave_counts[ml]++;
        walk->visit(walk->context, walk->letters, number_of_letters + 1,
                    word_index);
        const uint32_t arc_index = kwg_node_arc_index(node);
        if (number_of_letters + 1 < RACK_SIZE - 1 && arc_index != 0) {
          pool_shift_klv_walk_siblings(walk, arc_index, word_index + 1,
                                       number_of_letters + 1);
        }
        walk->leave_counts[ml]--;
      }
    }
    if (kwg_node_is_end(node)) {
      return;
    }
    word_index +=
        klv->word_counts[node_index] - klv->word_counts[node_index + 1];
  }
}

// Whether leaves that hold ml are left out for a pool of pool_counts: the
// pool has none of the letter. With NULL pool_counts no letter is left out.
static inline bool pool_shift_leaves_out_letter(const int *pool_counts,
                                                int ml) {
  return pool_counts != NULL && pool_counts[ml] == 0;
}

// A leave of one tile and the leaves that extend it: the walk's units, each
// of which may be walked on its own thread. node_index is the tile's node and
// word_index the index of the one-tile leave.
typedef struct PoolShiftKlvWalkUnit {
  MachineLetter ml;
  uint32_t node_index;
  uint32_t word_index;
} PoolShiftKlvWalkUnit;

// Sets the walk, with no visit or context, and its units, in the order of
// their indices. Returns the number of units.
static int pool_shift_klv_walk_set(PoolShiftKlvWalk *walk, const KLV *klv,
                                   const LetterDistribution *ld,
                                   const int *pool_counts,
                                   PoolShiftKlvWalkUnit *units) {
  walk->klv = klv;
  walk->ld_size = ld_get_size(ld);
  for (int ml = 0; ml < walk->ld_size; ml++) {
    walk->max_counts[ml] = pool_shift_leaves_out_letter(pool_counts, ml)
                               ? 0
                               : ld_get_dist(ld, (MachineLetter)ml);
    walk->leave_counts[ml] = 0;
  }
  walk->visit = NULL;
  walk->context = NULL;
  const KWG *kwg = klv_get_kwg(klv);
  uint32_t node_index = kwg_get_dawg_root_node_index(kwg);
  if (node_index == 0) {
    return 0;
  }
  // The root's siblings, as pool_shift_klv_walk_siblings passes over them.
  int number_of_units = 0;
  uint32_t word_index = 0;
  uint64_t seen_letters = 0;
  for (;; node_index++) {
    const uint32_t node = kwg_node(kwg, node_index);
    const uint32_t ml = kwg_node_tile(node);
    if (ml < (uint32_t)walk->ld_size &&
        (seen_letters & ((uint64_t)1 << ml)) == 0) {
      seen_letters |= (uint64_t)1 << ml;
      if (walk->max_counts[ml] > 0) {
        units[number_of_units].ml = (MachineLetter)ml;
        units[number_of_units].node_index = node_index;
        units[number_of_units].word_index = word_index;
        number_of_units++;
      }
    }
    if (kwg_node_is_end(node)) {
      return number_of_units;
    }
    word_index +=
        klv->word_counts[node_index] - klv->word_counts[node_index + 1];
  }
}

// Visits the leaves of a unit, with visit and context, on a copy of the walk.
static void pool_shift_klv_walk_unit(const PoolShiftKlvWalk *shared_walk,
                                     const PoolShiftKlvWalkUnit *unit,
                                     PoolShiftLeaveVisit visit, void *context) {
  PoolShiftKlvWalk walk = *shared_walk;
  walk.visit = visit;
  walk.context = context;
  walk.letters[0] = unit->ml;
  walk.leave_counts[unit->ml] = 1;
  visit(context, walk.letters, 1, unit->word_index);
  const uint32_t arc_index =
      kwg_node_arc_index(kwg_node(klv_get_kwg(walk.klv), unit->node_index));
  if (1 < RACK_SIZE - 1 && arc_index != 0) {
    pool_shift_klv_walk_siblings(&walk, arc_index, unit->word_index + 1, 1);
  }
}

typedef struct PoolShiftBuild {
  const LetterDistribution *ld;
  PoolShiftTable *table;
  // klv_get_leave_value of every leave of up to RACK_SIZE - 1 tiles the
  // letter distribution can deal, by rank (pool_shift_table_get_leave_row).
  const Equity *value_by_rank;
  Rack leave;
  // The leave's tiles, highest letter first.
  MachineLetter letters[RACK_SIZE];
  // Whether a tile the leave holds every copy of takes the leave's own value
  // rather than 0, so that its weight moves the leave's value by nothing.
  bool fill_exhausted;
  // The tiles of the pool the table is for, or NULL: a leave that holds a
  // letter the pool has none of gets no row, and no row a value for one.
  const int *pool_counts;
} PoolShiftBuild;

typedef struct PoolShiftRankFill {
  const PoolShiftTable *table;
  const KLV *klv;
  Equity *value_by_rank;
} PoolShiftRankFill;

static void pool_shift_set_rank_value(void *context,
                                      const MachineLetter *letters,
                                      int number_of_letters,
                                      uint32_t klv_index) {
  const PoolShiftRankFill *fill = context;
  // The rank takes the letters highest first.
  uint32_t rank = 0;
  for (int place = 0; place < number_of_letters; place++) {
    rank +=
        fill->table
            ->rank_increments[place][letters[number_of_letters - 1 - place]];
  }
  fill->value_by_rank[rank] = klv_get_indexed_leave_value(fill->klv, klv_index);
}

// Fills the row of the build's leave, of leave_size tiles: the value of the
// leave with one more of each tile it does not hold every copy of. With the
// leave's tiles m_0 >= ... >= m_(k-1), one more t goes to place p, the
// number of them above t, and the leave with it has rank
//   sum_(i < p) rank_increments[i][m_i] + rank_increments[p][t]
//     + sum_(i >= p) rank_increments[i + 1][m_i].
static void pool_shift_build_fill_row(const PoolShiftBuild *build, uint32_t row,
                                      int leave_size) {
  const PoolShiftTable *table = build->table;
  Equity *values = table->values + (size_t)row * (size_t)table->stride;
  uint32_t rank_before[RACK_SIZE];
  uint32_t rank_after[RACK_SIZE];
  rank_before[0] = 0;
  for (int place = 0; place < leave_size; place++) {
    rank_before[place + 1] =
        rank_before[place] +
        table->rank_increments[place][build->letters[place]];
  }
  rank_after[leave_size] = 0;
  for (int place = leave_size - 1; place >= 0; place--) {
    rank_after[place] =
        rank_after[place + 1] +
        table->rank_increments[place + 1][build->letters[place]];
  }
  int place = 0;
  for (int ml = table->ld_size - 1; ml >= 0; ml--) {
    while (place < leave_size && build->letters[place] > ml) {
      place++;
    }
    if (pool_shift_leaves_out_letter(build->pool_counts, ml)) {
      continue;
    }
    if (rack_get_letter(&build->leave, (MachineLetter)ml) >=
        ld_get_dist(build->ld, (MachineLetter)ml)) {
      if (build->fill_exhausted) {
        values[ml] = klv_get_leave_value(table->klv, &build->leave);
      }
      continue;
    }
    values[ml] = build->value_by_rank[rank_before[place] +
                                      table->rank_increments[place][ml] +
                                      rank_after[place]];
  }
}

// Fills the row at rank and the rows of leaves formed by adding tiles
// at or below max_ml.
static void pool_shift_build_walk(PoolShiftBuild *build, uint32_t rank,
                                  int max_ml) {
  const PoolShiftTable *table = build->table;
  const int leave_size = rack_get_total_letters(&build->leave);
  pool_shift_build_fill_row(build, rank, leave_size);
  if (leave_size == POOL_SHIFT_MAX_LEAVE_SIZE) {
    return;
  }
  for (int ml = max_ml; ml >= 0; ml--) {
    if (pool_shift_leaves_out_letter(build->pool_counts, ml) ||
        rack_get_letter(&build->leave, (MachineLetter)ml) >=
            ld_get_dist(build->ld, (MachineLetter)ml)) {
      continue;
    }
    const uint32_t extended_rank =
        rank + table->rank_increments[leave_size][ml];
    rack_add_letter(&build->leave, (MachineLetter)ml);
    build->letters[leave_size] = (MachineLetter)ml;
    pool_shift_build_walk(build, extended_rank, ml);
    rack_take_letter(&build->leave, (MachineLetter)ml);
  }
}

// The binomial term and the distance to the next size block depend only on
// the alphabet and the old size. Folding them together also gives ranks of
// the larger subsets used as played tiles in a census lookup. Returns the
// number of leaves of up to RACK_SIZE - 1 tiles, which have the ranks below
// it.
static size_t pool_shift_table_set_numbering(PoolShiftTable *table) {
  uint64_t size_counts[RACK_SIZE];
  size_counts[0] = 1;
  table->number_of_rows = 0;
  uint64_t number_of_ranks = 0;
  for (int size = 0; size < RACK_SIZE; size++) {
    if (size > 0) {
      size_counts[size] = size_counts[size - 1] *
                          (uint64_t)(table->ld_size + size - 1) /
                          (uint64_t)size;
    }
    if (size <= POOL_SHIFT_MAX_LEAVE_SIZE) {
      table->number_of_rows += (uint32_t)size_counts[size];
    }
    number_of_ranks += size_counts[size];
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
  return (size_t)number_of_ranks;
}

// The rows' values are the KLV's values of leaves of up to RACK_SIZE - 1
// tiles: one walk of the KLV puts them in an array by rank, from which each
// row reads its own.
// LOCAL MEASUREMENT SWITCH.
bool pool_leaves_fill_exhausted = true;

typedef struct PoolShiftRankFillBatch {
  const PoolShiftKlvWalk *walk;
  const PoolShiftKlvWalkUnit *units;
  PoolShiftRankFill *rank_fill;
} PoolShiftRankFillBatch;

static void pool_shift_rank_fill_run_unit(void *context, int unit_idx) {
  const PoolShiftRankFillBatch *batch = context;
  pool_shift_klv_walk_unit(batch->walk, &batch->units[unit_idx],
                           pool_shift_set_rank_value, batch->rank_fill);
}

// The rows of the leaves whose highest letter is a unit's: a unit for each
// letter.
typedef struct PoolShiftBuildBatch {
  const PoolShiftBuild *build;
  MachineLetter unit_letters[MAX_ALPHABET_SIZE];
} PoolShiftBuildBatch;

static void pool_shift_build_run_unit(void *context, int unit_idx) {
  const PoolShiftBuildBatch *batch = context;
  PoolShiftBuild build = *batch->build;
  const MachineLetter ml = batch->unit_letters[unit_idx];
  rack_add_letter(&build.leave, ml);
  build.letters[0] = ml;
  pool_shift_build_walk(&build, build.table->rank_increments[0][ml], ml);
}

static PoolShiftTable *
pool_shift_table_create_with_fill(const KLV *klv, const LetterDistribution *ld,
                                  bool fill_exhausted, const int *pool_counts,
                                  PoolShiftCrew *crew) {
  PoolShiftTable *table = malloc_or_die(sizeof(PoolShiftTable));
  table->instance_id =
      (uint64_t)atomic_fetch_add(&pool_shift_tables_created, 1) + 1;
  table->klv_instance_id = klv_get_instance_id(klv);
  table->klv_mutation_counter = klv_get_mutation_counter(klv);
  table->ld_hash = pool_shift_get_ld_hash(ld);
  table->ld_size = ld_get_size(ld);
  table->stride = (table->ld_size + 3) & ~3;
  const size_t number_of_ranks = pool_shift_table_set_numbering(table);
  table->values = calloc_or_die(
      (size_t)table->number_of_rows * (size_t)table->stride, sizeof(Equity));
  table->klv = klv;
  table->exact_values = NULL;
  table->has_reference_pool = false;
  table->reference_exact_offset = 0;
  // A leave the KLV does not have is worth 0, as klv_get_leave_value says.
  Equity *value_by_rank = calloc_or_die(number_of_ranks, sizeof(Equity));
  PoolShiftRankFill rank_fill = {
      .table = table,
      .klv = klv,
      .value_by_rank = value_by_rank,
  };
  PoolShiftKlvWalk walk;
  PoolShiftKlvWalkUnit walk_units[MAX_ALPHABET_SIZE];
  const int number_of_walk_units =
      pool_shift_klv_walk_set(&walk, klv, ld, pool_counts, walk_units);
  PoolShiftRankFillBatch rank_fill_batch = {
      .walk = &walk,
      .units = walk_units,
      .rank_fill = &rank_fill,
  };
  pool_shift_crew_run(crew, pool_shift_rank_fill_run_unit, &rank_fill_batch,
                      number_of_walk_units);
  PoolShiftBuild build = {
      .ld = ld,
      .table = table,
      .value_by_rank = value_by_rank,
      .fill_exhausted = fill_exhausted,
      .pool_counts = pool_counts,
  };
  rack_set_dist_size_and_reset(&build.leave, table->ld_size);
  // The empty leave's row, then the rows of the leaves with tiles, by their
  // highest letter as pool_shift_build_walk extends the empty leave.
  pool_shift_build_fill_row(&build, 0, 0);
  PoolShiftBuildBatch build_batch = {.build = &build};
  int number_of_build_units = 0;
  for (int ml = table->ld_size - 1; ml >= 0; ml--) {
    if (POOL_SHIFT_MAX_LEAVE_SIZE == 0 ||
        pool_shift_leaves_out_letter(pool_counts, ml) ||
        ld_get_dist(ld, (MachineLetter)ml) == 0) {
      continue;
    }
    build_batch.unit_letters[number_of_build_units] = (MachineLetter)ml;
    number_of_build_units++;
  }
  pool_shift_crew_run(crew, pool_shift_build_run_unit, &build_batch,
                      number_of_build_units);
  free(value_by_rank);
  return table;
}

PoolShiftTable *pool_shift_table_create(const KLV *klv,
                                        const LetterDistribution *ld) {
  return pool_shift_table_create_with_fill(klv, ld, false, NULL, NULL);
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
                      ? pool_shift_divide(numerator + denominator / 2,
                                          denominator)
                      : -pool_shift_divide(denominator / 2 - numerator,
                                           denominator));
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

// pool_shift_get_exact_leave_value for a leave of RACK_SIZE - 1 tiles. One
// tile is drawn, so each unseen letter gives one full rack, and the rack's
// index is the sum of three parts: the index terms of the leave's tiles
// before the letter, taken with the drawn tile still to come; the letter's
// own term with one more tile; and the terms of the leave's tiles after it.
// The first and last change only at the leave's letters, so one pass over
// the letters visits every rack, with no walk and no division. The racks,
// their indices and the sums are the walk's (pool_shift_exact_walk), so the
// value is the same.
static bool pool_shift_get_one_draw_leave_value(const PoolShiftTable *table,
                                                const int *unseen_counts,
                                                const Rack *leave,
                                                Equity *value) {
  const RackValueTable *rvt = table->exact_values;
  // The terms of all the leave's tiles, as they are once the tile is drawn.
  uint32_t index_after = 0;
  int tiles_from = RACK_SIZE - 1;
  for (int ml = 0; ml < table->ld_size; ml++) {
    const int leave_count = rack_get_letter(leave, (MachineLetter)ml);
    if (leave_count != 0) {
      index_after += rvt->rank_offsets[ml][tiles_from][leave_count];
      tiles_from -= leave_count;
    }
  }
  uint32_t index_before = 0;
  tiles_from = RACK_SIZE - 1;
  int64_t draws = 0;
  int64_t value_sum = 0;
  for (int ml = 0; ml < table->ld_size; ml++) {
    const int leave_count = rack_get_letter(leave, (MachineLetter)ml);
    // The letter's terms while the drawn tile is still to come.
    const uint32_t *undrawn_offsets = rvt->rank_offsets[ml][tiles_from + 1];
    if (leave_count != 0) {
      index_after -= rvt->rank_offsets[ml][tiles_from][leave_count];
    }
    const int unseen = unseen_counts[ml];
    if (unseen != 0) {
      const int32_t stored = rack_value_table_read_stored(
          rvt, index_before + undrawn_offsets[leave_count + 1] + index_after);
      if (stored != RACK_VALUE_TABLE_UNVALUED) {
        draws += unseen;
        value_sum += (int64_t)unseen * (int64_t)stored;
      }
    }
    if (leave_count != 0) {
      index_before += undrawn_offsets[leave_count];
      tiles_from -= leave_count;
    }
  }
  if (draws == 0) {
    return false;
  }
  const int64_t half = draws / 2;
  *value = (Equity)(value_sum >= 0 ? (value_sum + half) / draws
                                   : -((half - value_sum) / draws));
  return true;
}

// Sets *value to the exact value of leave, of fewer than RACK_SIZE tiles,
// for a mover who sees unseen_counts, and returns true, or returns false if
// no draw gives a rack the table values.
static bool pool_shift_get_exact_leave_value(const PoolShiftTable *table,
                                             const int *unseen_counts,
                                             const Rack *leave, Equity *value) {
  if (rack_get_total_letters(leave) == RACK_SIZE - 1) {
    return pool_shift_get_one_draw_leave_value(table, unseen_counts, leave,
                                               value);
  }
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

// The exact values of every leave of one rack at once (see
// pool_shift_fill_rack). Each leave's own walk would visit every full rack
// that contains it, so a rack's leaves together visit most full racks many
// times. Instead one walk visits each full rack the leaves can reach once,
// with the weight of its tiles of letters the rack does not hold, and adds
// its value to a sum kept by its counts of the rack's letters. Each leave's
// sum is then the sum over those counts, weighted by the ways to draw the
// rest of the rack's letters. Every sum is of the same integer terms as the
// leave's own walk, so the values are the same.

// A way for a full rack to end: its last tiles, of up to
// POOL_SHIFT_RACK_TAIL_TILES, in the order of the letters. The racks that
// share the tiles before them are added in one pass over the tails, where a
// walk down to each rack would spend most of its time on the steps between
// racks.
typedef struct PoolShiftRackTail {
  // What the tail's tiles add to the rack's index.
  uint32_t rack_offset;
  // The ways to draw its tiles of the letters the rack does not hold.
  int64_t ways;
} PoolShiftRackTail;

// The tails of one number of tiles, in the order of their first letters:
// those with no letter of the rack (plain) apart from the others (held), and
// for each letter the first tail of each kind whose letters are that letter
// or later ones.
typedef struct PoolShiftRackTails {
  int number_of_plain;
  int number_of_held;
  PoolShiftRackTail *plain;
  PoolShiftRackTail *held;
  int first_plain[MAX_ALPHABET_SIZE + 1];
  int first_held[MAX_ALPHABET_SIZE + 1];
  // held_sum_offsets[capacity][tail_idx]: what a held tail's tiles of the
  // rack's letters add to a sum index (see slot_offsets), for each capacity
  // of at least the tails' number of tiles.
  int32_t *held_sum_offsets[RACK_SIZE + 1];
} PoolShiftRackTails;

typedef struct PoolShiftRackSums {
  const RackValueTable *rvt;
  // The letters held or unseen, in order, the most of each a full rack can
  // hold here, and the most of all those from each letter on.
  int number_of_letters;
  MachineLetter letters[MAX_ALPHABET_SIZE];
  int max_counts[MAX_ALPHABET_SIZE];
  int max_from[MAX_ALPHABET_SIZE + 1];
  // Each letter's place among the rack's letters, or -1.
  int rack_slots[MAX_ALPHABET_SIZE];
  // letter_ways[letter_idx][count]: the ways to draw count tiles of a letter
  // the rack does not hold, or 1 for a letter it holds, whose draws the
  // leaves weight.
  int64_t letter_ways[MAX_ALPHABET_SIZE][RACK_SIZE + 1];
  // By number of tiles, from 1.
  PoolShiftRackTails tails[POOL_SHIFT_RACK_TAIL_TILES + 1];
  // The rack's distinct letters: the most of each a full rack can hold
  // here, and slot_ways[slot][drawn], the ways to draw drawn tiles of it.
  int number_of_slots;
  int slot_max[RACK_SIZE];
  int64_t slot_ways[RACK_SIZE][RACK_SIZE + 1];
  // Counts of the rack's letters, numbered like the pool leaves: the sum over
  // slots of slot_offsets[slot][capacity][count], where capacity is
  // RACK_SIZE less the counts of the slots before.
  int slot_offsets[RACK_SIZE][RACK_SIZE + 1][RACK_SIZE + 1];
  int number_of_sums;
  // By counts of the rack's letters: the draws of the other letters that
  // give a valued full rack, and the sum of those racks' values, each rack
  // counted once for every draw that gives it.
  int64_t *draws;
  int64_t *value_sums;
} PoolShiftRackSums;

// A walk over the tails of one number of tiles: the tail so far has
// number_of_held_letters letters of the rack, with these slots and counts.
typedef struct PoolShiftRackTailWalk {
  const PoolShiftRackSums *sums;
  PoolShiftRackTails *tails;
  int tiles;
  int number_of_held_letters;
  int held_slots[POOL_SHIFT_RACK_TAIL_TILES + 1];
  int held_counts[POOL_SHIFT_RACK_TAIL_TILES + 1];
} PoolShiftRackTailWalk;

// Counts the walk's tail, and stores it once the tails have their arrays.
static void pool_shift_rack_tail_walk_add(PoolShiftRackTailWalk *walk,
                                          uint32_t rack_offset, int64_t ways) {
  PoolShiftRackTails *tails = walk->tails;
  if (walk->number_of_held_letters == 0) {
    if (tails->plain != NULL) {
      tails->plain[tails->number_of_plain].rack_offset = rack_offset;
      tails->plain[tails->number_of_plain].ways = ways;
    }
    tails->number_of_plain++;
    return;
  }
  if (tails->held != NULL) {
    tails->held[tails->number_of_held].rack_offset = rack_offset;
    tails->held[tails->number_of_held].ways = ways;
    for (int capacity = walk->tiles; capacity <= RACK_SIZE; capacity++) {
      int sum_offset = 0;
      int capacity_left = capacity;
      for (int held_idx = 0; held_idx < walk->number_of_held_letters;
           held_idx++) {
        const int count = walk->held_counts[held_idx];
        sum_offset += walk->sums->slot_offsets[walk->held_slots[held_idx]]
                                              [capacity_left][count];
        capacity_left -= count;
      }
      tails->held_sum_offsets[capacity][tails->number_of_held] = sum_offset;
    }
  }
  tails->number_of_held++;
}

// Adds every tail that has, after the tiles that give it rack_offset and
// ways, at least one tile of letter_idx among its tiles_left tiles left.
static void pool_shift_rack_tail_walk_letter(PoolShiftRackTailWalk *walk,
                                             int letter_idx, int tiles_left,
                                             uint32_t rack_offset,
                                             int64_t ways) {
  const PoolShiftRackSums *sums = walk->sums;
  const MachineLetter ml = sums->letters[letter_idx];
  const int slot = sums->rack_slots[letter_idx];
  const int max_after = sums->max_from[letter_idx + 1];
  const int min_count = tiles_left - max_after > 1 ? tiles_left - max_after : 1;
  const int max_count = sums->max_counts[letter_idx] < tiles_left
                            ? sums->max_counts[letter_idx]
                            : tiles_left;
  for (int count = min_count; count <= max_count; count++) {
    const uint32_t offset = sums->rvt->rank_offsets[ml][tiles_left][count];
    if (offset == UINT32_MAX) {
      continue;
    }
    if (slot >= 0) {
      walk->held_slots[walk->number_of_held_letters] = slot;
      walk->held_counts[walk->number_of_held_letters] = count;
      walk->number_of_held_letters++;
    }
    const int64_t next_ways = ways * sums->letter_ways[letter_idx][count];
    if (count == tiles_left) {
      pool_shift_rack_tail_walk_add(walk, rack_offset + offset, next_ways);
    } else {
      for (int next_idx = letter_idx + 1; next_idx < sums->number_of_letters;
           next_idx++) {
        pool_shift_rack_tail_walk_letter(walk, next_idx, tiles_left - count,
                                         rack_offset + offset, next_ways);
      }
    }
    if (slot >= 0) {
      walk->number_of_held_letters--;
    }
  }
}

// Walks the tails of tiles tiles: to count them while the tails have no
// arrays, then to store them.
static void pool_shift_rack_tails_walk(const PoolShiftRackSums *sums,
                                       PoolShiftRackTails *tails, int tiles) {
  PoolShiftRackTailWalk walk;
  walk.sums = sums;
  walk.tails = tails;
  walk.tiles = tiles;
  walk.number_of_held_letters = 0;
  tails->number_of_plain = 0;
  tails->number_of_held = 0;
  for (int letter_idx = 0; letter_idx < sums->number_of_letters; letter_idx++) {
    tails->first_plain[letter_idx] = tails->number_of_plain;
    tails->first_held[letter_idx] = tails->number_of_held;
    pool_shift_rack_tail_walk_letter(&walk, letter_idx, tiles, 0, 1);
  }
  tails->first_plain[sums->number_of_letters] = tails->number_of_plain;
  tails->first_held[sums->number_of_letters] = tails->number_of_held;
}

// Sets the ways of the letters, whose unseen counts are letter_unseen, and
// the tails: the letters and the slot offsets are set.
static void pool_shift_rack_sums_set_tails(PoolShiftRackSums *sums,
                                           const int *letter_unseen) {
  for (int letter_idx = 0; letter_idx < sums->number_of_letters; letter_idx++) {
    const int unseen = letter_unseen[letter_idx];
    // C(unseen, count), carried from one count to the next.
    int64_t ways = 1;
    for (int count = 0; count <= RACK_SIZE; count++) {
      sums->letter_ways[letter_idx][count] =
          sums->rack_slots[letter_idx] < 0 ? ways : 1;
      ways = ways * (unseen - count) / (count + 1);
    }
  }
  for (int tiles = 1; tiles <= POOL_SHIFT_RACK_TAIL_TILES; tiles++) {
    PoolShiftRackTails *tails = &sums->tails[tiles];
    tails->plain = NULL;
    tails->held = NULL;
    pool_shift_rack_tails_walk(sums, tails, tiles);
    // At least one of each, so that no array is empty.
    const size_t plain_size = (size_t)tails->number_of_plain + 1;
    const size_t held_size = (size_t)tails->number_of_held + 1;
    tails->plain = malloc_or_die(plain_size * sizeof(PoolShiftRackTail));
    tails->held = malloc_or_die(held_size * sizeof(PoolShiftRackTail));
    tails->held_sum_offsets[0] =
        malloc_or_die(held_size * (RACK_SIZE + 1) * sizeof(int32_t));
    for (int capacity = 1; capacity <= RACK_SIZE; capacity++) {
      tails->held_sum_offsets[capacity] =
          tails->held_sum_offsets[capacity - 1] + held_size;
    }
    pool_shift_rack_tails_walk(sums, tails, tiles);
  }
}

static void pool_shift_rack_sums_free_tails(PoolShiftRackSums *sums) {
  for (int tiles = 1; tiles <= POOL_SHIFT_RACK_TAIL_TILES; tiles++) {
    free(sums->tails[tiles].held_sum_offsets[0]);
    free(sums->tails[tiles].held);
    free(sums->tails[tiles].plain);
  }
}

// Adds the full racks that have, after the tiles that give them rack_index,
// sum_index, capacity and ways, tiles tiles left (from 1 to
// POOL_SHIFT_RACK_TAIL_TILES) for letter_idx and the letters after it.
static inline void pool_shift_rack_sums_add_tails(PoolShiftRackSums *sums,
                                                  int letter_idx, int tiles,
                                                  uint32_t rack_index,
                                                  int sum_index, int capacity,
                                                  int64_t ways) {
  const RackValueTable *rvt = sums->rvt;
  const PoolShiftRackTails *tails = &sums->tails[tiles];
  const PoolShiftRackTail *plain = tails->plain;
  const int number_of_plain = tails->number_of_plain;
  int64_t draws = 0;
  int64_t value_sum = 0;
  for (int tail_idx = tails->first_plain[letter_idx];
       tail_idx < number_of_plain; tail_idx++) {
    const int32_t stored = rack_value_table_read_stored(
        rvt, rack_index + plain[tail_idx].rack_offset);
    if (stored != RACK_VALUE_TABLE_UNVALUED) {
      draws += plain[tail_idx].ways;
      value_sum += plain[tail_idx].ways * (int64_t)stored;
    }
  }
  sums->draws[sum_index] += ways * draws;
  sums->value_sums[sum_index] += ways * value_sum;
  const PoolShiftRackTail *held = tails->held;
  const int32_t *held_sum_offsets = tails->held_sum_offsets[capacity];
  const int number_of_held = tails->number_of_held;
  for (int tail_idx = tails->first_held[letter_idx]; tail_idx < number_of_held;
       tail_idx++) {
    const int32_t stored = rack_value_table_read_stored(
        rvt, rack_index + held[tail_idx].rack_offset);
    if (stored != RACK_VALUE_TABLE_UNVALUED) {
      const int tail_sum_index = sum_index + held_sum_offsets[tail_idx];
      const int64_t tail_ways = ways * held[tail_idx].ways;
      sums->draws[tail_sum_index] += tail_ways;
      sums->value_sums[tail_sum_index] += tail_ways * (int64_t)stored;
    }
  }
}

// Adds every full rack that has, after the tiles that give it rack_index,
// sum_index, capacity and ways, tiles tiles (more than
// POOL_SHIFT_RACK_TAIL_TILES) left for letter_idx and the letters after it:
// a step for each letter the rack holds, none for the letters between, and
// none for the racks' last tiles (pool_shift_rack_sums_add_tails).
static void pool_shift_rack_sums_walk(PoolShiftRackSums *sums, int letter_idx,
                                      int tiles, uint32_t rack_index,
                                      int sum_index, int capacity,
                                      int64_t ways) {
  for (int next_idx = letter_idx; next_idx < sums->number_of_letters;
       next_idx++) {
    const MachineLetter ml = sums->letters[next_idx];
    const int slot = sums->rack_slots[next_idx];
    const int max_after = sums->max_from[next_idx + 1];
    const int min_count = tiles - max_after > 1 ? tiles - max_after : 1;
    const int max_count =
        sums->max_counts[next_idx] < tiles ? sums->max_counts[next_idx] : tiles;
    for (int count = min_count; count <= max_count; count++) {
      const uint32_t offset = sums->rvt->rank_offsets[ml][tiles][count];
      if (offset == UINT32_MAX) {
        continue;
      }
      int next_sum_index = sum_index;
      int next_capacity = capacity;
      if (slot >= 0) {
        next_sum_index += sums->slot_offsets[slot][capacity][count];
        next_capacity -= count;
      }
      const int64_t next_ways = ways * sums->letter_ways[next_idx][count];
      const int tiles_left = tiles - count;
      if (tiles_left == 0) {
        const int32_t stored =
            rack_value_table_read_stored(sums->rvt, rack_index + offset);
        if (stored != RACK_VALUE_TABLE_UNVALUED) {
          sums->draws[next_sum_index] += next_ways;
          sums->value_sums[next_sum_index] += next_ways * (int64_t)stored;
        }
      } else if (tiles_left <= POOL_SHIFT_RACK_TAIL_TILES) {
        pool_shift_rack_sums_add_tails(sums, next_idx + 1, tiles_left,
                                       rack_index + offset, next_sum_index,
                                       next_capacity, next_ways);
      } else {
        pool_shift_rack_sums_walk(sums, next_idx + 1, tiles_left,
                                  rack_index + offset, next_sum_index,
                                  next_capacity, next_ways);
      }
    }
  }
}

// Adds to *draws and *value_sums the sums of every count of the rack's
// letters from slot on that can complete leave_counts, weighted by the ways
// to draw the difference.
static void pool_shift_rack_sums_gather(const PoolShiftRackSums *sums,
                                        const int *leave_counts, int slot,
                                        int sum_index, int capacity,
                                        int64_t ways, int64_t *draws,
                                        int64_t *value_sums) {
  if (slot == sums->number_of_slots) {
    *draws += ways * sums->draws[sum_index];
    *value_sums += ways * sums->value_sums[sum_index];
    return;
  }
  // A count the unseen tiles cannot give has no ways.
  const int kept = leave_counts[slot];
  const int max_count =
      sums->slot_max[slot] < capacity ? sums->slot_max[slot] : capacity;
  const int64_t *slot_ways = sums->slot_ways[slot];
  for (int count = kept; count <= max_count; count++) {
    const int64_t count_ways = slot_ways[count - kept];
    if (count_ways == 0) {
      break;
    }
    pool_shift_rack_sums_gather(
        sums, leave_counts, slot + 1,
        sum_index + sums->slot_offsets[slot][capacity][count], capacity - count,
        ways * count_ways, draws, value_sums);
  }
}

// Sets result->shifts to what pool_shift_get_exact_shift gives each of the
// result's leaves, for a mover who holds rack and sees unseen_counts.
static void pool_shift_fill_exact_shifts(const PoolShiftTable *table,
                                         const int *unseen_counts,
                                         const Rack *rack,
                                         PoolShiftRack *result) {
  PoolShiftRackSums *sums = malloc_or_die(sizeof(PoolShiftRackSums));
  sums->rvt = table->exact_values;
  sums->number_of_letters = 0;
  sums->number_of_slots = 0;
  int slot_of_ml[MAX_ALPHABET_SIZE];
  int letter_unseen[MAX_ALPHABET_SIZE];
  for (int ml = 0; ml < table->ld_size; ml++) {
    const int held = rack_get_letter(rack, (MachineLetter)ml);
    slot_of_ml[ml] = -1;
    if (held == 0 && unseen_counts[ml] == 0) {
      continue;
    }
    int max_count = held + unseen_counts[ml];
    if (max_count > RACK_SIZE) {
      max_count = RACK_SIZE;
    }
    const int letter_idx = sums->number_of_letters++;
    sums->letters[letter_idx] = (MachineLetter)ml;
    letter_unseen[letter_idx] = unseen_counts[ml];
    sums->max_counts[letter_idx] = max_count;
    sums->rack_slots[letter_idx] = -1;
    if (held != 0) {
      const int slot = sums->number_of_slots++;
      slot_of_ml[ml] = slot;
      sums->rack_slots[letter_idx] = slot;
      sums->slot_max[slot] = max_count;
      // C(unseen, drawn), carried from one count to the next.
      int64_t ways = 1;
      for (int drawn = 0; drawn <= RACK_SIZE; drawn++) {
        sums->slot_ways[slot][drawn] = ways;
        ways = ways * (unseen_counts[ml] - drawn) / (drawn + 1);
      }
    }
  }
  sums->max_from[sums->number_of_letters] = 0;
  for (int letter_idx = sums->number_of_letters - 1; letter_idx >= 0;
       letter_idx--) {
    sums->max_from[letter_idx] =
        sums->max_from[letter_idx + 1] + sums->max_counts[letter_idx];
  }
  // completions[capacity]: the counts of the slots after the current one
  // that fit in capacity tiles.
  int completions[RACK_SIZE + 1];
  for (int capacity = 0; capacity <= RACK_SIZE; capacity++) {
    completions[capacity] = 1;
  }
  for (int slot = sums->number_of_slots - 1; slot >= 0; slot--) {
    int slot_completions[RACK_SIZE + 1];
    for (int capacity = 0; capacity <= RACK_SIZE; capacity++) {
      int offset = 0;
      slot_completions[capacity] = 0;
      for (int count = 0; count <= RACK_SIZE; count++) {
        sums->slot_offsets[slot][capacity][count] = offset;
        if (count <= capacity && count <= sums->slot_max[slot]) {
          offset += completions[capacity - count];
        }
      }
      slot_completions[capacity] = offset;
    }
    memcpy(completions, slot_completions, sizeof(completions));
  }
  sums->number_of_sums = completions[RACK_SIZE];
  sums->draws = calloc_or_die((size_t)sums->number_of_sums, sizeof(int64_t));
  sums->value_sums =
      calloc_or_die((size_t)sums->number_of_sums, sizeof(int64_t));
  pool_shift_rack_sums_set_tails(sums, letter_unseen);
  pool_shift_rack_sums_walk(sums, 0, RACK_SIZE, 0, 0, RACK_SIZE, 1);

  Rack leave;
  for (int idx = 0; idx < result->count; idx++) {
    result->shifts[idx] = 0;
    if (result->sizes[idx] >= RACK_SIZE) {
      continue;
    }
    rack_set_dist_size_and_reset(&leave, table->ld_size);
    int leave_counts[RACK_SIZE] = {0};
    for (int bit_idx = 0; bit_idx < RACK_SIZE; bit_idx++) {
      if ((result->map_indices[idx] & (1 << bit_idx)) != 0) {
        const MachineLetter ml = result->bit_tiles[bit_idx];
        rack_add_letter(&leave, ml);
        leave_counts[slot_of_ml[ml]]++;
      }
    }
    int64_t draws = 0;
    int64_t value_sum = 0;
    pool_shift_rack_sums_gather(sums, leave_counts, 0, 0, RACK_SIZE, 1, &draws,
                                &value_sum);
    if (draws == 0) {
      continue;
    }
    // Rounded as pool_shift_get_exact_leave_value rounds.
    const int64_t half = draws / 2;
    const Equity exact_value =
        (Equity)(value_sum >= 0 ? (value_sum + half) / draws
                                : -((half - value_sum) / draws));
    result->shifts[idx] = exact_value - klv_get_leave_value(table->klv, &leave);
  }
  pool_shift_rack_sums_free_tails(sums);
  free(sums->value_sums);
  free(sums->draws);
  free(sums);
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
  // The most of each letter a rack walked holds: the letter distribution's
  // count, or 0 for a letter left out (see pool_leaves_get_pool_counts).
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

// The pool's tiles for the letters to leave out, or NULL to leave none out.
// A pool with the mover's rack holds every tile off the board, so every rack
// of the position and of any position after it is made of its tiles: a leave
// that holds a letter the pool has none of is never read, and a tile the pool
// has none of has a weight of 0 (pool_shift_fill_weights). Such leaves keep
// the stock KLV's values and get no rows, and no row gets a value for such a
// tile. Without the mover's rack, the mover may hold any letter.
static const int *pool_leaves_get_pool_counts(const int *unseen_counts,
                                              const Rack *mover_rack) {
  return mover_rack == NULL ? NULL : unseen_counts;
}

PoolLeaves *pool_leaves_create_by_walk(const PoolShiftTable *table,
                                       const LetterDistribution *ld,
                                       const Bag *bag,
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
  const int *pool_counts =
      pool_leaves_get_pool_counts(walk->unseen_counts, mover_rack);
  for (int ml = 0; ml < walk->ld_size; ml++) {
    walk->dist_counts[ml] = pool_shift_leaves_out_letter(pool_counts, ml)
                                ? 0
                                : ld_get_dist(ld, (MachineLetter)ml);
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
  pool_leaves->table = pool_shift_table_create_with_fill(
      pool_leaves->klv, ld, pool_leaves_fill_exhausted, pool_counts, NULL);
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

// The leaves of one pool by a transform of the full racks' values, one letter
// at a time, in place of the walk above, which splits every full rack into
// every leave and draw.
//
// Let S(L) and N(L) be the sum of values and the number of draws that the
// walk gives leave L: over the full racks R >= L the table values, of
// prod_t C(unseen_t, R_t - L_t) times V(R), or times 1. The weight is a
// product over letters, so S is a product of one-letter transforms, each
// acting on one letter's count: from the counts of a rack to those of a
// leave, S'(.., l, ..) = sum over r >= l of C(unseen, r - l) * S(.., r, ..).
// The transforms of different letters commute. A state whose counts total
// RACK_SIZE holds the counts of a rack R in every letter, transformed or
// not, so its value is V(R) at every step: only the states of up to
// RACK_SIZE - 1 tiles, which are the leaves, are stored, and the others are
// read from the table when they are a source.
//
// The sums are integers and are added in int64 (see
// pool_leaves_create_by_transform for why the result equals the walk's
// doubles).
typedef struct PoolLeaveSums {
  int64_t value_sum;
  int64_t draws;
} PoolLeaveSums;

typedef struct PoolLeavesTransform {
  const RackValueTable *rvt;
  int ld_size;
  int unseen_counts[MAX_ALPHABET_SIZE];
  // See pool_leaves_get_pool_counts.
  const int *pool_counts;
  // The most of each letter a leave or a rack read holds: the letter
  // distribution's count, or 0 for a letter left out.
  int dist_counts[MAX_ALPHABET_SIZE];
  // That, summed over the letters from ml on.
  int dist_from[MAX_ALPHABET_SIZE + 1];
  // ways[ml][drawn] is C(unseen_counts[ml], drawn).
  int64_t ways[MAX_ALPHABET_SIZE][RACK_SIZE + 1];
  // The states are numbered among the leaves of up to RACK_SIZE - 1 tiles
  // that the letter distribution can deal, less those with a letter left
  // out: a leave's index is the sum over
  // its letters of leave_offsets[ml][capacity][count], where capacity is
  // RACK_SIZE - 1 less the tiles of the letters before ml. The leaves that
  // agree on the letters before ml form one block, laid out by ml and
  // capacity alone, of one sub-block for each count of ml.
  uint32_t leave_offsets[MAX_ALPHABET_SIZE][RACK_SIZE][RACK_SIZE];
  // For a last tile, contiguous for the loops over it: the rack offset
  // (rank_offsets[ml][1][1], or UINT32_MAX for a letter with no tiles), and
  // leave_offsets[ml][capacity][1] by capacity.
  uint32_t one_tile_rack_offsets[MAX_ALPHABET_SIZE];
  uint32_t one_tile_leave_offsets[RACK_SIZE][MAX_ALPHABET_SIZE];
  uint32_t number_of_leaves;
  // block_sizes[ml][capacity]: the states of a block of up to capacity tiles
  // of the letters from ml on.
  uint32_t block_sizes[MAX_ALPHABET_SIZE + 1][RACK_SIZE];
  PoolLeaveSums *sums;
  // The largest |V(R)| of the racks read.
  int32_t max_abs_value;
} PoolLeavesTransform;

// Sets the pool, the bag, opponent_rack and mover_rack as in
// pool_leaves_create_by_walk, and returns its number of tiles.
static int pool_leaves_transform_set_pool(PoolLeavesTransform *transform,
                                          const PoolShiftTable *table,
                                          const LetterDistribution *ld,
                                          const Bag *bag,
                                          const Rack *opponent_rack,
                                          const Rack *mover_rack) {
  transform->rvt = table->exact_values;
  transform->ld_size = table->ld_size;
  int pool_size = pool_shift_get_unseen_counts(table, bag, opponent_rack,
                                               transform->unseen_counts);
  if (mover_rack != NULL) {
    for (int ml = 0; ml < transform->ld_size; ml++) {
      const int held = rack_get_letter(mover_rack, (MachineLetter)ml);
      transform->unseen_counts[ml] += held;
      pool_size += held;
    }
  }
  transform->pool_counts =
      pool_leaves_get_pool_counts(transform->unseen_counts, mover_rack);
  transform->dist_from[transform->ld_size] = 0;
  for (int ml = transform->ld_size - 1; ml >= 0; ml--) {
    transform->dist_counts[ml] =
        pool_shift_leaves_out_letter(transform->pool_counts, ml)
            ? 0
            : ld_get_dist(ld, (MachineLetter)ml);
    transform->dist_from[ml] =
        transform->dist_from[ml + 1] + transform->dist_counts[ml];
  }
  return pool_size;
}

// The most draws of any leave: the largest C(pool_size, k) for k up to
// RACK_SIZE, as the draws of k tiles from a pool, weighted by letter as
// above, number C(pool_size, k) (Vandermonde). UINT64_MAX if that does not
// fit.
static uint64_t pool_leaves_get_max_draws(int pool_size) {
  uint64_t draws = 1;
  uint64_t max_draws = 1;
  for (int tiles = 1; tiles <= RACK_SIZE && tiles <= pool_size; tiles++) {
    const uint64_t factor = (uint64_t)pool_size - (uint64_t)tiles + 1;
    if (draws > UINT64_MAX / factor) {
      return UINT64_MAX;
    }
    draws = draws * factor / (uint64_t)tiles;
    if (draws > max_draws) {
      max_draws = draws;
    }
  }
  return max_draws;
}

// Sets ways, which must fit (see pool_leaves_create_by_transform), and the
// leave offsets.
static void pool_leaves_transform_set_tables(PoolLeavesTransform *transform) {
  for (int ml = 0; ml < transform->ld_size; ml++) {
    const int unseen = transform->unseen_counts[ml];
    int64_t ways = 1;
    for (int drawn = 0; drawn <= RACK_SIZE; drawn++) {
      transform->ways[ml][drawn] = drawn <= unseen ? ways : 0;
      ways = ways * (unseen - drawn) / (drawn + 1);
    }
  }
  // dealable[capacity]: the leaves of up to capacity tiles that the letters
  // after the current one can deal. After the last letter, the empty leave.
  uint32_t dealable[RACK_SIZE];
  for (int capacity = 0; capacity < RACK_SIZE; capacity++) {
    dealable[capacity] = 1;
    transform->block_sizes[transform->ld_size][capacity] = 1;
  }
  for (int ml = transform->ld_size - 1; ml >= 0; ml--) {
    uint32_t dealable_with_letter[RACK_SIZE];
    for (int capacity = 0; capacity < RACK_SIZE; capacity++) {
      uint32_t offset = 0;
      for (int count = 0; count < RACK_SIZE; count++) {
        transform->leave_offsets[ml][capacity][count] = offset;
        if (count <= capacity && count <= transform->dist_counts[ml]) {
          offset += dealable[capacity - count];
        }
      }
      dealable_with_letter[capacity] = offset;
      transform->block_sizes[ml][capacity] = offset;
    }
    memcpy(dealable, dealable_with_letter, sizeof(dealable));
  }
  transform->number_of_leaves = dealable[RACK_SIZE - 1];
  for (int ml = 0; ml < transform->ld_size; ml++) {
    transform->one_tile_rack_offsets[ml] =
        transform->dist_counts[ml] == 0
            ? UINT32_MAX
            : transform->rvt->rank_offsets[ml][1][1];
    for (int capacity = 0; capacity < RACK_SIZE; capacity++) {
      transform->one_tile_leave_offsets[capacity][ml] =
          transform->leave_offsets[ml][capacity][1];
    }
  }
}

// Adds ways times the full rack at rack_index, if the table values it, to
// sums, and returns the rack's |V|, or 0.
static inline int32_t pool_leaves_add_rack(const RackValueTable *rvt,
                                           uint32_t rack_index,
                                           PoolLeaveSums *sums, int64_t ways) {
  const int32_t stored = rack_value_table_read_stored(rvt, rack_index);
  if (stored == RACK_VALUE_TABLE_UNVALUED) {
    return 0;
  }
  sums->value_sum += ways * (int64_t)stored;
  sums->draws += ways;
  return stored < 0 ? -stored : stored;
}

// Keeps the largest |V| read.
static inline void pool_leaves_note_value(PoolLeavesTransform *transform,
                                          int32_t magnitude) {
  if (magnitude > transform->max_abs_value) {
    transform->max_abs_value = magnitude;
  }
}

// pool_leaves_add_racks for one tile: T is one tile of a letter from ml on.
static inline void
pool_leaves_add_one_tile_racks(PoolLeavesTransform *transform, int ml,
                               uint32_t rack_index, int capacity,
                               uint32_t leave_index, int64_t ways) {
  const RackValueTable *rvt = transform->rvt;
  PoolLeaveSums *sums = transform->sums + leave_index;
  const uint32_t *rack_offsets = transform->one_tile_rack_offsets;
  const uint32_t *leave_offsets = transform->one_tile_leave_offsets[capacity];
  int32_t max_abs_value = transform->max_abs_value;
  for (int last_ml = ml; last_ml < transform->ld_size; last_ml++) {
    const uint32_t rack_offset = rack_offsets[last_ml];
    if (rack_offset == UINT32_MAX) {
      continue;
    }
    const int32_t magnitude = pool_leaves_add_rack(
        rvt, rack_index + rack_offset, sums + leave_offsets[last_ml], ways);
    max_abs_value = magnitude > max_abs_value ? magnitude : max_abs_value;
  }
  transform->max_abs_value = max_abs_value;
}

// For every T of exactly tiles (at least 1) tiles of the letters from ml
// on, adds ways times the full rack at rack_index plus T to the state of T
// in the block at leave_index, of up to capacity (at least tiles) tiles of
// those letters. T is enumerated by its lowest letter and that letter's
// count; the last tile or two are done here, without a call per rack.
static void pool_leaves_add_racks(PoolLeavesTransform *transform, int ml,
                                  int tiles, uint32_t rack_index, int capacity,
                                  uint32_t leave_index, int64_t ways) {
  if (tiles == 1) {
    pool_leaves_add_one_tile_racks(transform, ml, rack_index, capacity,
                                   leave_index, ways);
    return;
  }
  const RackValueTable *rvt = transform->rvt;
  for (int first_ml = ml;
       first_ml < transform->ld_size && transform->dist_from[first_ml] >= tiles;
       first_ml++) {
    const int dist = transform->dist_counts[first_ml];
    const int max_count = tiles < dist ? tiles : dist;
    // The letters after first_ml hold the rest.
    const int fewest = tiles - transform->dist_from[first_ml + 1];
    const int min_count = fewest > 1 ? fewest : 1;
    const uint32_t *leave_offsets =
        transform->leave_offsets[first_ml][capacity];
    for (int count = min_count; count <= max_count; count++) {
      const uint32_t rack_offset = rvt->rank_offsets[first_ml][tiles][count];
      if (rack_offset == UINT32_MAX) {
        continue;
      }
      const int rest = tiles - count;
      if (rest == 0) {
        pool_leaves_note_value(
            transform,
            pool_leaves_add_rack(
                rvt, rack_index + rack_offset,
                &transform->sums[leave_index + leave_offsets[count]], ways));
      } else if (rest == 1) {
        pool_leaves_add_one_tile_racks(
            transform, first_ml + 1, rack_index + rack_offset, capacity - count,
            leave_index + leave_offsets[count], ways);
      } else {
        pool_leaves_add_racks(transform, first_ml + 1, rest,
                              rack_index + rack_offset, capacity - count,
                              leave_index + leave_offsets[count], ways);
      }
    }
  }
}

// Adds ways times every state of the block at source_index, of up to
// source_capacity tiles of the letters from ml on, to the state of the same
// leave in the block at dest_index, of up to dest_capacity (more) tiles.
static void pool_leaves_add_states(PoolLeavesTransform *transform, int ml,
                                   int source_capacity, uint32_t source_index,
                                   int dest_capacity, uint32_t dest_index,
                                   int64_t ways) {
  PoolLeaveSums *sums = transform->sums;
  sums[dest_index].value_sum += ways * sums[source_index].value_sum;
  sums[dest_index].draws += ways * sums[source_index].draws;
  for (int first_ml = ml; first_ml < transform->ld_size; first_ml++) {
    const int dist = transform->dist_counts[first_ml];
    const int max_count = source_capacity < dist ? source_capacity : dist;
    const uint32_t *source_offsets =
        transform->leave_offsets[first_ml][source_capacity];
    const uint32_t *dest_offsets =
        transform->leave_offsets[first_ml][dest_capacity];
    for (int count = 1; count <= max_count; count++) {
      const uint32_t source = source_index + source_offsets[count];
      const uint32_t dest = dest_index + dest_offsets[count];
      if (count == source_capacity) {
        // A block of one state.
        sums[dest].value_sum += ways * sums[source].value_sum;
        sums[dest].draws += ways * sums[source].draws;
      } else {
        pool_leaves_add_states(transform, first_ml + 1, source_capacity - count,
                               source, dest_capacity - count, dest, ways);
      }
    }
  }
}

// The block of one leave of RACK_SIZE - 1 tiles, none of them ml or later:
// a draw of one of those letters makes it a full rack.
static void pool_leaves_transform_long_leave(PoolLeavesTransform *transform,
                                             int ml, uint32_t leave_index,
                                             uint32_t rack_index) {
  const RackValueTable *rvt = transform->rvt;
  const uint32_t *rack_offsets = transform->one_tile_rack_offsets;
  // Summed here, not through memory: every rack adds to the one state.
  PoolLeaveSums sums = transform->sums[leave_index];
  int32_t max_abs_value = transform->max_abs_value;
  for (int last_ml = ml; last_ml < transform->ld_size; last_ml++) {
    const uint32_t rack_offset = rack_offsets[last_ml];
    if (transform->unseen_counts[last_ml] == 0 || rack_offset == UINT32_MAX) {
      continue;
    }
    const int32_t magnitude = pool_leaves_add_rack(
        rvt, rack_index + rack_offset, &sums, transform->ways[last_ml][1]);
    max_abs_value = magnitude > max_abs_value ? magnitude : max_abs_value;
  }
  transform->sums[leave_index] = sums;
  transform->max_abs_value = max_abs_value;
}

// The transform of letter ml on the block at leave_index (see
// pool_leaves_transform_block), of at least one tile of capacity, once the
// letters after ml are done in each of its sub-blocks: for the sub-blocks of
// first_count of ml and more, in place. A sub-block takes from those of
// higher counts, which still hold their values before ml.
static void pool_leaves_transform_letter(PoolLeavesTransform *transform, int ml,
                                         int capacity, uint32_t leave_index,
                                         uint32_t rack_index, int first_count) {
  const int unseen = transform->unseen_counts[ml];
  if (unseen == 0) {
    return;
  }
  const RackValueTable *rvt = transform->rvt;
  const uint32_t *leave_offsets = transform->leave_offsets[ml][capacity];
  const int rack_tiles = capacity + 1;
  const int dist = transform->dist_counts[ml];
  const int max_count = capacity < dist ? capacity : dist;
  const int max_rack_count = rack_tiles < dist ? rack_tiles : dist;
  for (int count = first_count; count <= max_count; count++) {
    const uint32_t dest_index = leave_index + leave_offsets[count];
    const int max_source =
        count + unseen < max_rack_count ? count + unseen : max_rack_count;
    for (int source = count + 1; source <= max_source; source++) {
      const int64_t ways = transform->ways[ml][source - count];
      if (source <= capacity) {
        pool_leaves_add_states(transform, ml + 1, capacity - source,
                               leave_index + leave_offsets[source],
                               capacity - count, dest_index, ways);
      }
      // The sources of RACK_SIZE tiles.
      const uint32_t rack_offset = rvt->rank_offsets[ml][rack_tiles][source];
      if (rack_offset == UINT32_MAX) {
        continue;
      }
      if (source == rack_tiles) {
        pool_leaves_note_value(
            transform,
            pool_leaves_add_rack(rvt, rack_index + rack_offset,
                                 &transform->sums[dest_index], ways));
      } else {
        pool_leaves_add_racks(transform, ml + 1, rack_tiles - source,
                              rack_index + rack_offset, capacity - count,
                              dest_index, ways);
      }
    }
  }
}

// Applies the transforms of the letters from ml on to the block at
// leave_index: the states that agree on the counts of the letters before ml,
// which are still a rack's and make up rack_index, and hold up to capacity
// more tiles. Letters after ml go first, sub-block by sub-block, then ml.
static void pool_leaves_transform_block(PoolLeavesTransform *transform, int ml,
                                        int capacity, uint32_t leave_index,
                                        uint32_t rack_index) {
  if (capacity == 0) {
    pool_leaves_transform_long_leave(transform, ml, leave_index, rack_index);
    return;
  }
  if (ml == transform->ld_size) {
    return;
  }
  const RackValueTable *rvt = transform->rvt;
  const uint32_t *leave_offsets = transform->leave_offsets[ml][capacity];
  const int rack_tiles = capacity + 1;
  const int dist = transform->dist_counts[ml];
  const int max_count = capacity < dist ? capacity : dist;
  for (int count = 0; count <= max_count; count++) {
    uint32_t rack_offset = 0;
    if (count != 0) {
      rack_offset = rvt->rank_offsets[ml][rack_tiles][count];
      // No rack has this count: the sub-block stays 0.
      if (rack_offset == UINT32_MAX) {
        continue;
      }
    }
    pool_leaves_transform_block(transform, ml + 1, capacity - count,
                                leave_index + leave_offsets[count],
                                rack_index + rack_offset);
  }
  pool_leaves_transform_letter(transform, ml, capacity, leave_index, rack_index,
                               0);
}

// The transform on several threads. Blocks too large for one unit are split
// into their sub-blocks, which are disjoint and take nothing from one
// another: the smaller ones are transformed whole, a unit each, in one
// batch. The letter of each block that was split is then transformed in the
// order the blocks were split in, innermost first. Its sub-block of none of
// the letter, the largest, is done in pieces, a unit each: the states whose
// lowest letter after the block's is some letter, with some count of it. The
// pieces are disjoint, and take from the sub-blocks of more of the block's
// letter, which change only after.
typedef struct PoolLeavesBlock {
  int ml;
  int capacity;
  uint32_t leave_index;
  uint32_t rack_index;
  uint32_t size;
} PoolLeavesBlock;

// A piece of a split block's sub-block of none of its letter: the states
// whose lowest letter after the block's is first_ml, with count of it.
typedef struct PoolLeavesPiece {
  int first_ml;
  int count;
  uint32_t size;
} PoolLeavesPiece;

typedef struct PoolLeavesBlockList {
  PoolLeavesBlock *blocks;
  int number_of_blocks;
  int capacity;
} PoolLeavesBlockList;

static void pool_leaves_block_list_add(PoolLeavesBlockList *list, int ml,
                                       int capacity, uint32_t leave_index,
                                       uint32_t rack_index, uint32_t size) {
  if (list->number_of_blocks == list->capacity) {
    list->capacity = list->capacity == 0 ? 64 : list->capacity * 2;
    list->blocks = realloc_or_die(list->blocks, sizeof(PoolLeavesBlock) *
                                                    (size_t)list->capacity);
  }
  PoolLeavesBlock *block = &list->blocks[list->number_of_blocks];
  block->ml = ml;
  block->capacity = capacity;
  block->leave_index = leave_index;
  block->rack_index = rack_index;
  block->size = size;
  list->number_of_blocks++;
}

// Largest first, so that the threads finish together.
static int pool_leaves_compare_blocks(const void *left, const void *right) {
  const PoolLeavesBlock *left_block = left;
  const PoolLeavesBlock *right_block = right;
  if (left_block->size != right_block->size) {
    return left_block->size > right_block->size ? -1 : 1;
  }
  if (left_block->leave_index != right_block->leave_index) {
    return left_block->leave_index < right_block->leave_index ? -1 : 1;
  }
  return 0;
}

static int pool_leaves_compare_pieces(const void *left, const void *right) {
  const PoolLeavesPiece *left_piece = left;
  const PoolLeavesPiece *right_piece = right;
  if (left_piece->size != right_piece->size) {
    return left_piece->size > right_piece->size ? -1 : 1;
  }
  if (left_piece->first_ml != right_piece->first_ml) {
    return left_piece->first_ml < right_piece->first_ml ? -1 : 1;
  }
  return left_piece->count - right_piece->count;
}

// Splits the block as pool_leaves_transform_block recurses, down to blocks
// of fewer than min_split_size states.
static void pool_leaves_plan_block(const PoolLeavesTransform *transform,
                                   uint32_t min_split_size,
                                   PoolLeavesBlockList *whole_blocks,
                                   PoolLeavesBlockList *split_blocks, int ml,
                                   int capacity, uint32_t leave_index,
                                   uint32_t rack_index) {
  const uint32_t size = transform->block_sizes[ml][capacity];
  if (capacity == 0 || ml == transform->ld_size || size < min_split_size) {
    pool_leaves_block_list_add(whole_blocks, ml, capacity, leave_index,
                               rack_index, size);
    return;
  }
  const RackValueTable *rvt = transform->rvt;
  const uint32_t *leave_offsets = transform->leave_offsets[ml][capacity];
  const int rack_tiles = capacity + 1;
  const int dist = transform->dist_counts[ml];
  const int max_count = capacity < dist ? capacity : dist;
  for (int count = 0; count <= max_count; count++) {
    uint32_t rack_offset = 0;
    if (count != 0) {
      rack_offset = rvt->rank_offsets[ml][rack_tiles][count];
      if (rack_offset == UINT32_MAX) {
        continue;
      }
    }
    pool_leaves_plan_block(transform, min_split_size, whole_blocks,
                           split_blocks, ml + 1, capacity - count,
                           leave_index + leave_offsets[count],
                           rack_index + rack_offset);
  }
  pool_leaves_block_list_add(split_blocks, ml, capacity, leave_index,
                             rack_index, size);
}

// A batch of units over the transform: each unit works on a copy of it, for
// the largest |V| it reads, which goes to max_abs_values.
typedef struct PoolLeavesTransformBatch {
  const PoolLeavesTransform *transform;
  // The blocks transformed whole, or split_block and its pieces.
  const PoolLeavesBlock *blocks;
  PoolLeavesBlock split_block;
  const PoolLeavesPiece *pieces;
  int32_t *max_abs_values;
} PoolLeavesTransformBatch;

static void pool_leaves_transform_run_whole_block(void *context, int unit_idx) {
  const PoolLeavesTransformBatch *batch = context;
  PoolLeavesTransform transform = *batch->transform;
  transform.max_abs_value = 0;
  const PoolLeavesBlock *block = &batch->blocks[unit_idx];
  pool_leaves_transform_block(&transform, block->ml, block->capacity,
                              block->leave_index, block->rack_index);
  batch->max_abs_values[unit_idx] = transform.max_abs_value;
}

// pool_leaves_add_states for the states that hold count, at least 1, of
// first_ml and none of the letters from ml up to it.
static void pool_leaves_add_states_piece(PoolLeavesTransform *transform,
                                         int first_ml, int count,
                                         int source_capacity,
                                         uint32_t source_index,
                                         int dest_capacity, uint32_t dest_index,
                                         int64_t ways) {
  if (count > source_capacity || count > transform->dist_counts[first_ml]) {
    return;
  }
  PoolLeaveSums *sums = transform->sums;
  const uint32_t source =
      source_index + transform->leave_offsets[first_ml][source_capacity][count];
  const uint32_t dest =
      dest_index + transform->leave_offsets[first_ml][dest_capacity][count];
  if (count == source_capacity) {
    sums[dest].value_sum += ways * sums[source].value_sum;
    sums[dest].draws += ways * sums[source].draws;
  } else {
    pool_leaves_add_states(transform, first_ml + 1, source_capacity - count,
                           source, dest_capacity - count, dest, ways);
  }
}

// pool_leaves_add_racks for the T that hold count, at least 1, of first_ml
// and none of the letters from ml up to it.
static void pool_leaves_add_racks_piece(PoolLeavesTransform *transform,
                                        int first_ml, int count, int tiles,
                                        uint32_t rack_index, int capacity,
                                        uint32_t leave_index, int64_t ways) {
  const RackValueTable *rvt = transform->rvt;
  if (tiles == 1) {
    // As pool_leaves_add_one_tile_racks: T is one tile of first_ml.
    const uint32_t rack_offset = transform->one_tile_rack_offsets[first_ml];
    if (count != 1 || rack_offset == UINT32_MAX) {
      return;
    }
    pool_leaves_note_value(
        transform,
        pool_leaves_add_rack(
            rvt, rack_index + rack_offset,
            &transform
                 ->sums[leave_index +
                        transform->one_tile_leave_offsets[capacity][first_ml]],
            ways));
    return;
  }
  if (transform->dist_from[first_ml] < tiles) {
    return;
  }
  const int dist = transform->dist_counts[first_ml];
  const int max_count = tiles < dist ? tiles : dist;
  const int fewest = tiles - transform->dist_from[first_ml + 1];
  const int min_count = fewest > 1 ? fewest : 1;
  if (count < min_count || count > max_count) {
    return;
  }
  const uint32_t rack_offset = rvt->rank_offsets[first_ml][tiles][count];
  if (rack_offset == UINT32_MAX) {
    return;
  }
  const uint32_t *leave_offsets = transform->leave_offsets[first_ml][capacity];
  const int rest = tiles - count;
  if (rest == 0) {
    pool_leaves_note_value(
        transform,
        pool_leaves_add_rack(
            rvt, rack_index + rack_offset,
            &transform->sums[leave_index + leave_offsets[count]], ways));
  } else if (rest == 1) {
    pool_leaves_add_one_tile_racks(transform, first_ml + 1,
                                   rack_index + rack_offset, capacity - count,
                                   leave_index + leave_offsets[count], ways);
  } else {
    pool_leaves_add_racks(transform, first_ml + 1, rest,
                          rack_index + rack_offset, capacity - count,
                          leave_index + leave_offsets[count], ways);
  }
}

// One piece of the transform of the split block's letter for its sub-block
// of none of the letter.
static void pool_leaves_transform_run_piece(void *context, int unit_idx) {
  const PoolLeavesTransformBatch *batch = context;
  PoolLeavesTransform transform = *batch->transform;
  transform.max_abs_value = 0;
  const int first_ml = batch->pieces[unit_idx].first_ml;
  const int piece_count = batch->pieces[unit_idx].count;
  const int ml = batch->split_block.ml;
  const int capacity = batch->split_block.capacity;
  const uint32_t leave_index = batch->split_block.leave_index;
  const uint32_t rack_index = batch->split_block.rack_index;
  const RackValueTable *rvt = transform.rvt;
  const uint32_t *leave_offsets = transform.leave_offsets[ml][capacity];
  const int rack_tiles = capacity + 1;
  const int dist = transform.dist_counts[ml];
  const int unseen = transform.unseen_counts[ml];
  const int max_rack_count = rack_tiles < dist ? rack_tiles : dist;
  const int max_source = unseen < max_rack_count ? unseen : max_rack_count;
  const uint32_t dest_index = leave_index + leave_offsets[0];
  for (int source = 1; source <= max_source; source++) {
    const int64_t ways = transform.ways[ml][source];
    if (source <= capacity) {
      pool_leaves_add_states_piece(
          &transform, first_ml, piece_count, capacity - source,
          leave_index + leave_offsets[source], capacity, dest_index, ways);
    }
    const uint32_t rack_offset = rvt->rank_offsets[ml][rack_tiles][source];
    if (rack_offset == UINT32_MAX || source == rack_tiles) {
      continue;
    }
    pool_leaves_add_racks_piece(&transform, first_ml, piece_count,
                                rack_tiles - source, rack_index + rack_offset,
                                capacity, dest_index, ways);
  }
  batch->max_abs_values[unit_idx] = transform.max_abs_value;
}

// What the pieces leave of the transform of the split block's letter for its
// sub-block of none of the letter: the state of no tiles of the letters
// after it.
static void pool_leaves_transform_piece_rest(PoolLeavesTransform *transform,
                                             const PoolLeavesBlock *block) {
  const RackValueTable *rvt = transform->rvt;
  PoolLeaveSums *sums = transform->sums;
  const int ml = block->ml;
  const int capacity = block->capacity;
  const uint32_t *leave_offsets = transform->leave_offsets[ml][capacity];
  const int rack_tiles = capacity + 1;
  const int dist = transform->dist_counts[ml];
  const int unseen = transform->unseen_counts[ml];
  const int max_rack_count = rack_tiles < dist ? rack_tiles : dist;
  const int max_source = unseen < max_rack_count ? unseen : max_rack_count;
  const uint32_t dest_index = block->leave_index + leave_offsets[0];
  for (int source = 1; source <= max_source; source++) {
    const int64_t ways = transform->ways[ml][source];
    if (source <= capacity) {
      const uint32_t source_index = block->leave_index + leave_offsets[source];
      sums[dest_index].value_sum += ways * sums[source_index].value_sum;
      sums[dest_index].draws += ways * sums[source_index].draws;
    }
    const uint32_t rack_offset = rvt->rank_offsets[ml][rack_tiles][source];
    if (rack_offset != UINT32_MAX && source == rack_tiles) {
      pool_leaves_note_value(
          transform, pool_leaves_add_rack(rvt, block->rack_index + rack_offset,
                                          &sums[dest_index], ways));
    }
  }
}

static void pool_leaves_transform_note_values(PoolLeavesTransform *transform,
                                              const int32_t *max_abs_values,
                                              int number_of_values) {
  for (int value_idx = 0; value_idx < number_of_values; value_idx++) {
    pool_leaves_note_value(transform, max_abs_values[value_idx]);
  }
}

// The whole transform: pool_leaves_transform_block of every state, on the
// crew's threads when there is a crew.
static void pool_leaves_transform_all(PoolLeavesTransform *transform,
                                      PoolShiftCrew *crew) {
  if (crew == NULL) {
    pool_leaves_transform_block(transform, 0, RACK_SIZE - 1, 0, 0);
    return;
  }
  const uint32_t units_wanted = 4 * ((uint32_t)crew->number_of_helpers + 1);
  const uint32_t min_split_size =
      transform->number_of_leaves / units_wanted + 1;
  PoolLeavesBlockList whole_blocks = {0};
  PoolLeavesBlockList split_blocks = {0};
  pool_leaves_plan_block(transform, min_split_size, &whole_blocks,
                         &split_blocks, 0, RACK_SIZE - 1, 0, 0);
  qsort(whole_blocks.blocks, (size_t)whole_blocks.number_of_blocks,
        sizeof(PoolLeavesBlock), pool_leaves_compare_blocks);
  // The pieces of a block: at most RACK_SIZE - 1 counts of each letter.
  const int max_pieces = MAX_ALPHABET_SIZE * (RACK_SIZE - 1);
  const int max_units = whole_blocks.number_of_blocks > max_pieces
                            ? whole_blocks.number_of_blocks
                            : max_pieces;
  int32_t *max_abs_values = malloc_or_die(sizeof(int32_t) * (size_t)max_units);
  PoolLeavesPiece *pieces =
      malloc_or_die(sizeof(PoolLeavesPiece) * (size_t)max_pieces);
  PoolLeavesTransformBatch batch = {
      .transform = transform,
      .blocks = whole_blocks.blocks,
      .pieces = pieces,
      .max_abs_values = max_abs_values,
  };
  pool_shift_crew_run(crew, pool_leaves_transform_run_whole_block, &batch,
                      whole_blocks.number_of_blocks);
  pool_leaves_transform_note_values(transform, max_abs_values,
                                    whole_blocks.number_of_blocks);
  for (int split_idx = 0; split_idx < split_blocks.number_of_blocks;
       split_idx++) {
    const PoolLeavesBlock *block = &split_blocks.blocks[split_idx];
    if (transform->unseen_counts[block->ml] == 0) {
      continue;
    }
    int number_of_pieces = 0;
    for (int first_ml = block->ml + 1; first_ml < transform->ld_size;
         first_ml++) {
      const int dist = transform->dist_counts[first_ml];
      const int max_count = block->capacity < dist ? block->capacity : dist;
      for (int count = 1; count <= max_count; count++) {
        pieces[number_of_pieces].first_ml = first_ml;
        pieces[number_of_pieces].count = count;
        pieces[number_of_pieces].size =
            transform->block_sizes[first_ml + 1][block->capacity - count];
        number_of_pieces++;
      }
    }
    qsort(pieces, (size_t)number_of_pieces, sizeof(PoolLeavesPiece),
          pool_leaves_compare_pieces);
    batch.split_block = *block;
    pool_shift_crew_run(crew, pool_leaves_transform_run_piece, &batch,
                        number_of_pieces);
    pool_leaves_transform_note_values(transform, max_abs_values,
                                      number_of_pieces);
    pool_leaves_transform_piece_rest(transform, block);
    pool_leaves_transform_letter(transform, block->ml, block->capacity,
                                 block->leave_index, block->rack_index, 1);
  }
  free(pieces);
  free(max_abs_values);
  free(split_blocks.blocks);
  free(whole_blocks.blocks);
}

// The index of a leave, its letters in ascending order.
static uint32_t
pool_leaves_transform_get_index(const PoolLeavesTransform *transform,
                                const MachineLetter *letters,
                                int number_of_letters) {
  uint32_t leave_index = 0;
  int capacity = RACK_SIZE - 1;
  int letter_idx = 0;
  while (letter_idx < number_of_letters) {
    const MachineLetter ml = letters[letter_idx];
    int count = 1;
    while (letter_idx + count < number_of_letters &&
           letters[letter_idx + count] == ml) {
      count++;
    }
    leave_index += transform->leave_offsets[ml][capacity][count];
    capacity -= count;
    letter_idx += count;
  }
  return leave_index;
}

typedef struct PoolLeavesKlvFill {
  const PoolLeavesTransform *transform;
  KLV *klv;
  double empty_value;
  // Whether every KLV index came after the one before: in a KLV whose
  // every node accepts, they do, and no two leaves share an index.
  bool indices_increase;
  bool has_klv_index;
  uint32_t first_klv_index;
  uint32_t last_klv_index;
} PoolLeavesKlvFill;

// Sets the pool KLV's value of a leave, as pool_leaves_fill_klv does.
static void pool_leaves_set_klv_value(void *context,
                                      const MachineLetter *letters,
                                      int number_of_letters,
                                      uint32_t klv_index) {
  PoolLeavesKlvFill *fill = context;
  if (fill->has_klv_index && klv_index <= fill->last_klv_index) {
    fill->indices_increase = false;
  }
  if (!fill->has_klv_index) {
    fill->first_klv_index = klv_index;
  }
  fill->has_klv_index = true;
  fill->last_klv_index = klv_index;
  const PoolLeaveSums *sums =
      &fill->transform->sums[pool_leaves_transform_get_index(
          fill->transform, letters, number_of_letters)];
  if (sums->draws > 0) {
    fill->klv->leave_values[klv_index] = (Equity)llround(
        ((double)sums->value_sum / (double)sums->draws) - fill->empty_value);
  }
}

// The walk's units, each with a fill of its own for the indices it sees.
typedef struct PoolLeavesKlvFillBatch {
  const PoolShiftKlvWalk *walk;
  const PoolShiftKlvWalkUnit *units;
  PoolLeavesKlvFill *fills;
} PoolLeavesKlvFillBatch;

static void pool_leaves_klv_fill_run_unit(void *context, int unit_idx) {
  const PoolLeavesKlvFillBatch *batch = context;
  pool_shift_klv_walk_unit(batch->walk, &batch->units[unit_idx],
                           pool_leaves_set_klv_value, &batch->fills[unit_idx]);
}

// Builds what pool_leaves_create_by_walk builds, bit for bit, or returns
// NULL when the sums might not fit int64 or the walk's doubles might not be
// exact.
//
// Every partial sum the walk forms for a leave of k tiles fewer than a rack
// is at most max |V| times the draws of k tiles, C(pool_size, k): when that
// is below 2^53, every double the walk forms is an exact integer, so its
// sums do not depend on their order and equal the exact sums here, and the
// values computed from them below are the walk's. The KLV values come in
// the order of the KLV's word graph rather than the walk's, which is the
// same when no two leaves share an index.
static PoolLeaves *pool_leaves_create_by_transform(
    const PoolShiftTable *table, const LetterDistribution *ld, const Bag *bag,
    const Rack *opponent_rack, const Rack *mover_rack, int number_of_threads) {
  PoolLeavesTransform *transform = malloc_or_die(sizeof(PoolLeavesTransform));
  const int pool_size = pool_leaves_transform_set_pool(
      transform, table, ld, bag, opponent_rack, mover_rack);
  const uint64_t max_draws = pool_leaves_get_max_draws(pool_size);
  // With every |V| below 2^23, every sum and product below fits in 62 bits.
  if (max_draws >=
      (UINT64_C(1) << 62U) / (uint64_t)RACK_VALUE_TABLE_MAX_VALUE) {
    free(transform);
    return NULL;
  }
  pool_leaves_transform_set_tables(transform);
  transform->sums =
      calloc_or_die(transform->number_of_leaves, sizeof(PoolLeaveSums));
  transform->max_abs_value = 0;
  PoolShiftCrew *crew = pool_shift_crew_create(number_of_threads);
  pool_leaves_transform_all(transform, crew);
  if ((uint64_t)transform->max_abs_value * max_draws >= (UINT64_C(1) << 53U)) {
    pool_shift_crew_destroy(crew);
    free(transform->sums);
    free(transform);
    return NULL;
  }

  const KLV *stock = table->klv;
  const KWG *stock_kwg = klv_get_kwg(stock);
  KWG *kwg = kwg_create_empty();
  const int number_of_nodes = kwg_get_number_of_nodes(stock_kwg);
  kwg_allocate_nodes(kwg, (size_t)number_of_nodes);
  memcpy(kwg_get_mutable_nodes(kwg), kwg_get_mutable_nodes(stock_kwg),
         sizeof(uint32_t) * (size_t)number_of_nodes);
  KLV *klv = klv_create_zeroed_from_kwg(
      kwg, (int)klv_get_number_of_leaves(stock), klv_get_name(stock));
  memcpy(klv->leave_values, stock->leave_values,
         sizeof(Equity) * (size_t)klv_get_number_of_leaves(stock));
  const PoolLeaveSums *empty_sums = &transform->sums[0];
  const PoolLeavesKlvFill fill = {
      .transform = transform,
      .klv = klv,
      .empty_value = empty_sums->draws > 0 ? (double)empty_sums->value_sum /
                                                 (double)empty_sums->draws
                                           : 0.0,
      .indices_increase = true,
      .has_klv_index = false,
      .first_klv_index = 0,
      .last_klv_index = 0,
  };
  PoolShiftKlvWalk walk;
  PoolShiftKlvWalkUnit walk_units[MAX_ALPHABET_SIZE];
  const int number_of_walk_units = pool_shift_klv_walk_set(
      &walk, klv, ld, transform->pool_counts, walk_units);
  PoolLeavesKlvFill unit_fills[MAX_ALPHABET_SIZE];
  for (int unit_idx = 0; unit_idx < number_of_walk_units; unit_idx++) {
    unit_fills[unit_idx] = fill;
  }
  PoolLeavesKlvFillBatch fill_batch = {
      .walk = &walk,
      .units = walk_units,
      .fills = unit_fills,
  };
  pool_shift_crew_run(crew, pool_leaves_klv_fill_run_unit, &fill_batch,
                      number_of_walk_units);
  free(transform->sums);
  // The units are in the order of their indices: the indices increase over
  // the whole walk when they do within each unit and from each to the next.
  bool indices_increase = true;
  bool has_klv_index = false;
  uint32_t last_klv_index = 0;
  for (int unit_idx = 0; unit_idx < number_of_walk_units; unit_idx++) {
    const PoolLeavesKlvFill *unit_fill = &unit_fills[unit_idx];
    if (!unit_fill->has_klv_index) {
      continue;
    }
    if (!unit_fill->indices_increase ||
        (has_klv_index && unit_fill->first_klv_index <= last_klv_index)) {
      indices_increase = false;
    }
    has_klv_index = true;
    last_klv_index = unit_fill->last_klv_index;
  }
  if (!indices_increase) {
    pool_shift_crew_destroy(crew);
    klv_destroy(klv);
    free(transform);
    return NULL;
  }
  klv_mark_leave_values_mutated(klv);
  PoolLeaves *pool_leaves = malloc_or_die(sizeof(PoolLeaves));
  pool_leaves->klv = klv;
  pool_leaves->table = pool_shift_table_create_with_fill(
      klv, ld, pool_leaves_fill_exhausted, transform->pool_counts, crew);
  pool_shift_crew_destroy(crew);
  pool_leaves->table->has_reference_pool = true;
  pool_leaves->table->exact_values = table->exact_values;
  pool_leaves->table->reference_exact_offset =
      (Equity)llround(fill.empty_value);
  pool_leaves->table->reference_total = pool_size;
  memcpy(pool_leaves->table->reference_counts, transform->unseen_counts,
         sizeof(transform->unseen_counts));
  free(transform);
  return pool_leaves;
}

PoolLeaves *pool_leaves_create(const PoolShiftTable *table,
                               const LetterDistribution *ld, const Bag *bag,
                               const Rack *opponent_rack,
                               const Rack *mover_rack, int number_of_threads) {
  PoolLeaves *pool_leaves = pool_leaves_create_by_transform(
      table, ld, bag, opponent_rack, mover_rack, number_of_threads);
  if (pool_leaves == NULL) {
    pool_leaves =
        pool_leaves_create_by_walk(table, ld, bag, opponent_rack, mover_rack);
  }
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
  const bool is_exact = weight_milli == POOL_SHIFT_EXACT_WEIGHT_MILLI;
  if (!is_exact) {
    pool_shift_fill_weights(table, ld, bag, opponent_rack, weight_milli,
                            result->weights);
  }
  result->ranks[0] = 0;
  result->map_indices[0] = 0;
  result->sizes[0] = 0;
  size_t value_sources[1 << RACK_SIZE];
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
  if (is_exact) {
    int unseen_counts[MAX_ALPHABET_SIZE];
    pool_shift_get_unseen_counts(table, bag, opponent_rack, unseen_counts);
    // What is remembered by a rack's weights is remembered by its pool
    // (see PoolShiftRack): here they are the unseen tiles themselves.
    for (int ml = 0; ml < POOL_SHIFT_MAX_STRIDE; ml++) {
      result->weights[ml] = ml < table->ld_size ? unseen_counts[ml] : 0;
    }
    pool_shift_fill_exact_shifts(table, unseen_counts, rack, result);
    pool_shift_fill_rack_values(table, value_sources, result);
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
    result->shifts[idx] =
        size > POOL_SHIFT_MAX_LEAVE_SIZE
            ? 0
            : pool_shift_table_get_shift(table, result->weights,
                                         result->ranks[idx], size);
  }
  // Every row read here was read for a shift above.
  pool_shift_fill_rack_values(table, value_sources, result);
  if (!shifts_long_leaves) {
    return;
  }
  // A leave of RACK_SIZE - 1 tiles: its exact value less its value as a
  // leave, which the rack's values already hold.
  for (int idx = 0; idx < count; idx++) {
    if (result->sizes[idx] != RACK_SIZE - 1) {
      continue;
    }
    Rack leave;
    rack_set_dist_size_and_reset(&leave, table->ld_size);
    for (int bit_idx = 0; bit_idx < RACK_SIZE; bit_idx++) {
      if ((result->map_indices[idx] & (1 << bit_idx)) != 0) {
        rack_add_letter(&leave, result->bit_tiles[bit_idx]);
      }
    }
    Equity exact_value = 0;
    if (pool_shift_get_exact_leave_value(table, unseen_counts, &leave,
                                         &exact_value)) {
      result->shifts[idx] =
          exact_value - table->reference_exact_offset - result->values[idx];
    }
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
