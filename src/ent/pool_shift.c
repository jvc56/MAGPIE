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
#include "rack_value_table.h"
#include <assert.h>
#include <float.h>
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
  // The bits of a rack's stored value taken from the least a table holds
  // (see PoolShiftWideSums).
  POOL_SHIFT_STORED_VALUE_BITS = 24,
};

static_assert(RACK_VALUE_TABLE_MAX_VALUE - RACK_VALUE_TABLE_MIN_VALUE <
                  (1 << POOL_SHIFT_STORED_VALUE_BITS),
              "a stored value taken from the least fits its bits");

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
  table->klv = klv;
  table->exact_values = NULL;
  table->exact_sums_max_unseen = 0;
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

// The most draws of any leave from a pool of pool_size tiles: the largest
// C(pool_size, k) for k up to RACK_SIZE, as the draws of k tiles from a
// pool, each counted once for every way the pool gives its tiles, number
// C(pool_size, k) (Vandermonde). UINT64_MAX if that does not fit.
static uint64_t pool_shift_get_max_draws(int pool_size) {
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

// The largest value of any rack, either way. Reads the whole table.
static int pool_shift_get_max_abs_rack_value(const RackValueTable *rvt) {
  int max_abs_value = 0;
  const uint32_t number_of_racks = rack_value_table_get_number_of_racks(rvt);
  for (uint32_t rack_index = 0; rack_index < number_of_racks; rack_index++) {
    const int32_t stored = rack_value_table_read_stored(rvt, rack_index);
    if (stored == RACK_VALUE_TABLE_UNVALUED) {
      continue;
    }
    const int abs_value = stored < 0 ? -stored : stored;
    if (abs_value > max_abs_value) {
      max_abs_value = abs_value;
    }
  }
  return max_abs_value;
}

// The most unseen tiles, up to max_unseen, for which the sums of the exact
// leave values (pool_shift_exact_walk and PoolShiftRackSums) fit int64 when
// no rack's value is past max_abs_value either way. With max_draws the most
// draws of any leave, no sum of draws passes max_draws, no sum of values
// passes max_abs_value * max_draws, rounding adds at most max_draws / 2,
// and carrying a letter's ways from one count to the next multiplies at
// most max_draws by the letter's unseen tiles.
static int pool_shift_get_exact_sums_max_unseen(int max_abs_value,
                                                int max_unseen) {
  const uint64_t value_factor = (uint64_t)max_abs_value + 1;
  int fitting_unseen = 0;
  for (int unseen = 1; unseen <= max_unseen; unseen++) {
    const uint64_t max_draws = pool_shift_get_max_draws(unseen);
    const uint64_t max_factor =
        (uint64_t)unseen > value_factor ? (uint64_t)unseen : value_factor;
    if (max_draws > (uint64_t)INT64_MAX / max_factor) {
      break;
    }
    fitting_unseen = unseen;
  }
  return fitting_unseen;
}

bool pool_shift_table_set_exact_values(PoolShiftTable *table,
                                       const RackValueTable *rvt,
                                       const LetterDistribution *ld) {
  table->exact_values = NULL;
  table->exact_sums_max_unseen = 0;
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
  // For most distributions the sums fit whatever values a table holds. For
  // one with more tiles, the table's own values decide.
  const int total_tiles = ld_get_total_tiles(ld);
  table->exact_sums_max_unseen = pool_shift_get_exact_sums_max_unseen(
      RACK_VALUE_TABLE_MAX_VALUE, total_tiles);
  if (table->exact_sums_max_unseen < total_tiles) {
    table->exact_sums_max_unseen = pool_shift_get_exact_sums_max_unseen(
        pool_shift_get_max_abs_rack_value(rvt), total_tiles);
  }
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

// A leave's sums for a pool with more unseen tiles than the table's
// exact_sums_max_unseen, where pool_shift_exact_walk's might not fit int64.
// Each rack's value, taken from the least a table holds so that it is not
// negative, is split into digits of digit_bits bits, least first, and each
// digit has a sum of its own. With every leave's draws below 2^draw_bits and
// digit_bits at most 62 - draw_bits, every digit's sum is below 2^62.
typedef struct PoolShiftWideSums {
  int digit_bits;
  int number_of_digits;
  int64_t draws;
  int64_t digit_sums[POOL_SHIFT_STORED_VALUE_BITS];
} PoolShiftWideSums;

// pool_shift_exact_walk with the sums of PoolShiftWideSums.
static void pool_shift_exact_walk_wide(const PoolShiftExactWalk *walk,
                                       PoolShiftWideSums *sums, int letter_idx,
                                       int tiles_to_draw, uint32_t rack_index,
                                       int64_t ways) {
  if (tiles_to_draw == 0) {
    const int32_t stored = rack_value_table_read_stored(
        walk->rvt, rack_index + walk->leave_index_from[letter_idx]);
    if (stored == RACK_VALUE_TABLE_UNVALUED) {
      return;
    }
    sums->draws += ways;
    uint32_t digits = (uint32_t)(stored - RACK_VALUE_TABLE_MIN_VALUE);
    const uint32_t digit_mask = ((uint32_t)1 << sums->digit_bits) - 1;
    for (int digit_idx = 0; digit_idx < sums->number_of_digits; digit_idx++) {
      sums->digit_sums[digit_idx] += ways * (int64_t)(digits & digit_mask);
      digits >>= sums->digit_bits;
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
  // C(unseen, drawn), carried from one count to the next: the product is
  // the next count times its ways, at most RACK_SIZE times the most draws
  // of any leave.
  int64_t letter_ways = 1;
  for (int drawn = 0; drawn < min_drawn; drawn++) {
    letter_ways = letter_ways * (unseen - drawn) / (drawn + 1);
  }
  for (int drawn = min_drawn; drawn <= max_drawn; drawn++) {
    const int count = leave_count + drawn;
    const uint32_t offset =
        count == 0 ? 0 : walk->rvt->rank_offsets[ml][tiles][count];
    pool_shift_exact_walk_wide(walk, sums, letter_idx + 1,
                               tiles_to_draw - drawn, rack_index + offset,
                               ways * letter_ways);
    // Not past the last count, whose successor is not drawn and need not
    // fit.
    if (drawn < max_drawn) {
      letter_ways = letter_ways * (unseen - drawn) / (drawn + 1);
    }
  }
}

// Sets *value to the leave's value from its wide sums, rounded as
// pool_shift_get_exact_leave_value rounds the sums that fit, and returns
// true, or returns false if no draw gave a rack the table values.
static bool pool_shift_wide_sums_get_value(const PoolShiftWideSums *sums,
                                           Equity *value) {
  const int64_t draws = sums->draws;
  if (draws == 0) {
    return false;
  }
  // Long division from the top digit. The digits so far make
  // quotient * draws + remainder, and the remainder shifted up a digit plus
  // the next digit's sum is below draws * 2^(digit_bits + 1), at most 2^63.
  const int64_t digit_base = (int64_t)1 << sums->digit_bits;
  int64_t quotient = 0;
  int64_t remainder = 0;
  for (int digit_idx = sums->number_of_digits - 1; digit_idx >= 0;
       digit_idx--) {
    const int64_t dividend =
        remainder * digit_base + sums->digit_sums[digit_idx];
    quotient = quotient * digit_base + dividend / draws;
    remainder = dividend % draws;
  }
  // The sum of the values is floor_value * draws + remainder: rounded to
  // the nearest, and away from 0 from halfway.
  const int64_t floor_value = quotient + RACK_VALUE_TABLE_MIN_VALUE;
  const int64_t half = draws / 2;
  const bool rounds_up =
      floor_value >= 0 ? remainder + half >= draws : remainder > half;
  *value = (Equity)(floor_value + (rounds_up ? 1 : 0));
  return true;
}

// pool_shift_get_exact_leave_value by the wide sums, for a leave that draws
// tiles_to_draw tiles on a walk that is set up. Also returns false, leaving
// the leave without an exact value, for a pool so large that even these
// sums might not fit: with racks of 7 tiles, more than any bag holds.
static bool pool_shift_get_wide_leave_value(const PoolShiftExactWalk *walk,
                                            int tiles_to_draw, Equity *value) {
  const uint64_t max_draws = pool_shift_get_max_draws(walk->unseen_from[0]);
  int draw_bits = 0;
  while (draw_bits < 63 && (max_draws >> draw_bits) != 0) {
    draw_bits++;
  }
  if (draw_bits > 61 || max_draws > (uint64_t)INT64_MAX / (uint64_t)RACK_SIZE) {
    return false;
  }
  PoolShiftWideSums sums;
  sums.digit_bits = 62 - draw_bits;
  if (sums.digit_bits > POOL_SHIFT_STORED_VALUE_BITS) {
    sums.digit_bits = POOL_SHIFT_STORED_VALUE_BITS;
  }
  sums.number_of_digits =
      (POOL_SHIFT_STORED_VALUE_BITS + sums.digit_bits - 1) / sums.digit_bits;
  sums.draws = 0;
  for (int digit_idx = 0; digit_idx < sums.number_of_digits; digit_idx++) {
    sums.digit_sums[digit_idx] = 0;
  }
  pool_shift_exact_walk_wide(walk, &sums, 0, tiles_to_draw, 0, 1);
  return pool_shift_wide_sums_get_value(&sums, value);
}

// pool_shift_get_exact_leave_value for a leave of RACK_SIZE - 1 tiles. One
// tile is drawn, so each unseen letter gives one full rack, and the rack's
// index is the sum of three parts: the index terms of the leave's tiles
// before the letter, taken with the drawn tile still to come; the letter's
// own term with one more tile; and the terms of the leave's tiles after it.
// The first and last change only at the leave's letters, so one pass over
// the letters visits every rack, with no walk and no division. The racks,
// their indices and the sums are the walk's (pool_shift_exact_walk), so the
// value is the same. Its sums are at most a value times the unseen tiles,
// so they fit int64 for any pool.
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
  if (walk.unseen_from[0] > table->exact_sums_max_unseen) {
    return pool_shift_get_wide_leave_value(&walk, tiles_to_draw, value);
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
  // Counts of the rack's letters, numbered in the order of the slots: the sum
  // over slots of slot_offsets[slot][capacity][count], where capacity is
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

// pool_shift_fill_exact_shifts with each leave on its own walk: for a pool
// with more unseen tiles than the table's exact_sums_max_unseen, where the
// sums of PoolShiftRackSums might not fit int64.
static void pool_shift_fill_exact_shifts_by_leave(const PoolShiftTable *table,
                                                  const int *unseen_counts,
                                                  PoolShiftRack *result) {
  Rack leave;
  for (int idx = 0; idx < result->count; idx++) {
    rack_set_dist_size_and_reset(&leave, table->ld_size);
    for (int bit_idx = 0; bit_idx < RACK_SIZE; bit_idx++) {
      if ((result->map_indices[idx] & (1 << bit_idx)) != 0) {
        rack_add_letter(&leave, result->bit_tiles[bit_idx]);
      }
    }
    result->shifts[idx] =
        pool_shift_get_exact_shift(table, unseen_counts, &leave);
  }
}

// Sets result->shifts to what pool_shift_get_exact_shift gives each of the
// result's leaves, for a mover who holds rack and sees unseen_counts.
static void pool_shift_fill_exact_shifts(const PoolShiftTable *table,
                                         const int *unseen_counts,
                                         const Rack *rack,
                                         PoolShiftRack *result) {
  int unseen_total = 0;
  for (int ml = 0; ml < table->ld_size; ml++) {
    unseen_total += unseen_counts[ml];
  }
  if (unseen_total > table->exact_sums_max_unseen) {
    pool_shift_fill_exact_shifts_by_leave(table, unseen_counts, result);
    return;
  }
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
  if (weight_milli == POOL_SHIFT_EXACT_WEIGHT_MILLI) {
    int unseen_counts[MAX_ALPHABET_SIZE];
    pool_shift_get_unseen_counts(table, bag, opponent_rack, unseen_counts);
    return pool_shift_get_exact_shift(table, unseen_counts, leave);
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
