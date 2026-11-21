#ifndef POOL_SHIFT_H
#define POOL_SHIFT_H

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "bag.h"
#include "equity.h"
#include "klv.h"
#include "letter_distribution.h"
#include "rack.h"
#include "rack_value_table.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The pool shift: a correction to a leave's value for the tiles the mover
// will really draw from.
//
// A leave's value averages over draws from every pool the games it was
// trained on had, which is close to a full bag. The pool a play draws from,
// the bag and the opponent's rack as the mover sees them, is richer or poorer
// than that, and suits the leave better or worse. To first order in the
// difference, keeping a leave L and drawing to a full rack is worth
//
//   weight * (RACK_SIZE - |L|) * sum over tiles t of
//       (unseen_t / unseen - dist_t / total) * value(L + t)
//
// more than the leave's value says, where unseen_t of the unseen tiles are t
// and the distribution has dist_t of its total. value(L + t) is 0 where L
// already holds every t. The sum is over a difference of two distributions,
// so it is 0 for a pool in the distribution's proportions, and it is the
// same for every play that keeps L.
//
// The shift is added to the leave value of every play, exchange included, of
// a player whose weight is not 0, when the mover's rack is full and the bag
// holds at least RACK_SIZE tiles, for leaves of up to
// POOL_SHIFT_MAX_LEAVE_SIZE tiles: the empty leave has one, and a longer
// leave plus a tile is not a leave. It is computed in integers, so that
// every platform, and every place that computes it, gets the same
// millipoints.
//
// A PoolShiftTable holds value(L + t) for one KLV: a row of values by t for
// each leave L. A leave's row is computed from its tiles alone, with no walk
// of the KLV and no lookup: the rows of each leave size are in the order of
// the census lattice. For alphabet size n, number the tiles in reverse
// alphabet order (z is 0 in English). For a leave with those numbers
// a_0 <= ... <= a_(k-1), its rank is its size block's start plus
// sum_i C(a_i + i, i + 1). Adding the next tile requires one addition.
// All sizes have ranks. Only leaves through POOL_SHIFT_MAX_LEAVE_SIZE
// have rows of extension values.

enum {
  POOL_SHIFT_MAX_LEAVE_SIZE = RACK_SIZE - 2,
  // Rows are padded to a multiple of four values.
  POOL_SHIFT_MAX_STRIDE = (MAX_ALPHABET_SIZE + 3) & ~3,
  POOL_SHIFT_EMPTY_LEAVE_ROW = 0,
  // A tile's weight is its share difference times the player's weight, in
  // units of 2^-POOL_SHIFT_WEIGHT_BITS: fine enough that a shift is within a
  // millipoint of exact, and at the largest player's weight a tile's weight
  // still fits 32 bits and a shift's sum 64.
  POOL_SHIFT_WEIGHT_BITS = 24,
  // A player's weight is set in thousandths, up to this.
  POOL_SHIFT_MAX_WEIGHT_MILLI = 10000,
  // In place of a weight: the exact leave values (see below).
  POOL_SHIFT_EXACT_WEIGHT_MILLI = -1,
  // The rollouts of a player whose leave values are exact: the exact values
  // cost far too much for every ply of every rollout. A sim from a bag of at
  // least a rack builds the leaves of its pool once (see PoolLeaves), and
  // its rollouts' plies read those and shift them at
  // POOL_SHIFT_POOL_LEAVES_WEIGHT_MILLI, the first-order correction as
  // computed. Any other sim's plies shift the stock leaves at
  // POOL_SHIFT_EXACT_ROLLOUT_WEIGHT_MILLI, the weight that scored best in
  // static play.
  POOL_SHIFT_EXACT_ROLLOUT_WEIGHT_MILLI = 1200,
  POOL_SHIFT_POOL_LEAVES_WEIGHT_MILLI = 1000,
};

// The exact leave values: in place of the first-order shift, a leave L is
// valued as the mean value of the full rack it becomes, over every draw of
// RACK_SIZE - |L| tiles from the unseen tiles:
//
//   sum over draws D of prod_t C(unseen_t, D_t) * value(L + D)
//       / C(unseen, RACK_SIZE - |L|)
//
// where value is a RackValueTable's (rack_value_table.h), the full racks'
// values on the scale of the KLV's leaves. A rack the table has no value for
// is left out of the mean. The "shift" of a leave is then that mean less the
// KLV's value of the leave, so that everything that adds a shift to a leave
// value ends with the mean. It applies where the pool shift does, and to
// leaves of RACK_SIZE - 1 tiles too. The sums are in integers.

// The row of a leave without one.
#define POOL_SHIFT_NO_ROW UINT32_MAX

typedef struct PoolShiftTable {
  // Which build this is: unique for the life of the process, never 0. What
  // is remembered of a table's rows is remembered by it.
  uint64_t instance_id;
  // The KLV the table was built from, as it was then, and a hash of the
  // letter distribution's tile counts.
  uint64_t klv_instance_id;
  uint64_t klv_mutation_counter;
  uint64_t ld_hash;
  int ld_size;
  int stride;
  uint32_t number_of_rows;
  // Adding ml to a size-k subset whose letters are all >= ml advances
  // its rank by one size block plus the new tile's binomial term.
  uint32_t rank_increments[RACK_SIZE][MAX_ALPHABET_SIZE];
  // number_of_rows rows of stride values: values[row * stride + t] is the
  // KLV's value of the row's leave with one more t.
  Equity *values;
  // The KLV the table was built from, and the full racks' values for the
  // exact leave values: NULL until pool_shift_table_set_exact_values gives
  // them. Neither is owned.
  const KLV *klv;
  const RackValueTable *exact_values;
  // The pool the KLV's values are for, when they are not for the letter
  // distribution's (see PoolLeaves): the shift then corrects for the unseen
  // tiles' shares less this pool's.
  bool has_reference_pool;
  int reference_counts[MAX_ALPHABET_SIZE];
  int reference_total;
  // With exact_values, a leave of RACK_SIZE - 1 tiles, which has no row, is
  // shifted to its exact value for the unseen tiles, whatever the weight:
  // one draw fills it, so the first-order shift is exact. With a reference
  // pool, this is what the KLV's values are less than the exact values of
  // that pool, and is 0 otherwise.
  Equity reference_exact_offset;
} PoolShiftTable;

// The leaves of one pool: a KLV whose value of every leave is the leave's
// exact value for that pool (see above) less the empty leave's, so that an
// empty leave is worth 0 as in any KLV, and the pool shift table of that KLV,
// whose shifts correct for how the unseen tiles differ from that pool.
//
// A sim builds them for the tiles neither player has played, the bag and
// both racks, which both players know: the same leaves then serve either
// player's plies. Its rollouts' plies read leave values from the KLV and
// shift them by the table: a ply's mover sees that pool less their own rack
// and the few tiles since played, so the first-order shift has little left
// to correct.
typedef struct PoolLeaves {
  KLV *klv;
  PoolShiftTable *table;
} PoolLeaves;

// Builds the leaves of the pool of the bag, opponent_rack and mover_rack,
// from table's KLV and full racks' values, which it must have
// (pool_shift_table_set_exact_values). With a NULL mover_rack, the pool is
// the one the mover sees.
PoolLeaves *pool_leaves_create(const PoolShiftTable *table,
                               const LetterDistribution *ld, const Bag *bag,
                               const Rack *opponent_rack,
                               const Rack *mover_rack);

// Builds the same leaves by a walk over every way to split every full rack
// into a leave and a draw: far slower, and the definition the tests compare
// pool_leaves_create against. pool_leaves_create falls back to it when its
// own sums might not be exact, or when the KLV's word indices do not increase
// along its KWG.
PoolLeaves *pool_leaves_create_by_walk(const PoolShiftTable *table,
                                       const LetterDistribution *ld,
                                       const Bag *bag,
                                       const Rack *opponent_rack,
                                       const Rack *mover_rack);
void pool_leaves_destroy(PoolLeaves *pool_leaves);

PoolShiftTable *pool_shift_table_create(const KLV *klv,
                                        const LetterDistribution *ld);
void pool_shift_table_destroy(PoolShiftTable *table);

// Whether the table was built from this KLV as it is now.
static inline bool pool_shift_table_matches_klv(const PoolShiftTable *table,
                                                const KLV *klv) {
  return klv != NULL && table->klv_instance_id == klv_get_instance_id(klv) &&
         table->klv_mutation_counter == klv_get_mutation_counter(klv);
}

// Gives the table the full racks' values for the exact leave values, or
// takes them away with NULL. Returns false, taking them away, if rvt is not
// for the table's KLV and ld's tile counts.
bool pool_shift_table_set_exact_values(PoolShiftTable *table,
                                       const RackValueTable *rvt,
                                       const LetterDistribution *ld);

// Whether the table was built for this letter distribution's tile counts.
bool pool_shift_table_matches_ld(const PoolShiftTable *table,
                                 const LetterDistribution *ld);

// Whether the pool shift applies to the plays of a mover holding
// tiles_on_rack tiles, from a bag of real_bag_size tiles (see
// game_get_real_bag_size), with this table, which may be NULL, and weight.
static inline bool pool_shift_applies(const PoolShiftTable *table,
                                      const KLV *klv, int weight_milli,
                                      int tiles_on_rack, int real_bag_size) {
  return table != NULL && weight_milli != 0 && tiles_on_rack == RACK_SIZE &&
         real_bag_size >= RACK_SIZE &&
         pool_shift_table_matches_klv(table, klv) &&
         (weight_milli != POOL_SHIFT_EXACT_WEIGHT_MILLI ||
          table->exact_values != NULL);
}

// The row of a leave.
uint32_t pool_shift_table_get_leave_row(const PoolShiftTable *table,
                                        const Rack *leave);

// Sets weights[t], for each t below the table's stride, to the weight of t's
// values for a mover who sees the bag and opponent_rack as unseen. The
// weights are 0 when nothing is unseen.
void pool_shift_fill_weights(const PoolShiftTable *table,
                             const LetterDistribution *ld, const Bag *bag,
                             const Rack *opponent_rack, int weight_milli,
                             int32_t weights[POOL_SHIFT_MAX_STRIDE]);

// Asks for a row's values ahead of pool_shift_table_get_shift. A caller with
// many rows to read asks for all of them first, so that their cache misses
// overlap: the table is far larger than a processor's caches.
static inline void pool_shift_table_prefetch_row(const PoolShiftTable *table,
                                                 uint32_t row) {
#if defined(__has_builtin) && __has_builtin(__builtin_prefetch)
  const Equity *values = table->values + (size_t)row * (size_t)table->stride;
  __builtin_prefetch(values);
  if (table->stride > 16) {
    __builtin_prefetch(values + 16);
  }
#else
  (void)table;
  (void)row;
#endif
}

// The pool shift of keeping the leave of leave_size tiles at row, which is
// not POOL_SHIFT_NO_ROW, under weights (pool_shift_fill_weights).
static inline Equity pool_shift_table_get_shift(const PoolShiftTable *table,
                                                const int32_t *weights,
                                                uint32_t row, int leave_size) {
  const Equity *values = table->values + (size_t)row * (size_t)table->stride;
  int64_t sum = 0;
  for (int value_idx = 0; value_idx < table->stride; value_idx++) {
    sum += (int64_t)weights[value_idx] * (int64_t)values[value_idx];
  }
  sum *= RACK_SIZE - leave_size;
  const int64_t half = (int64_t)1 << (POOL_SHIFT_WEIGHT_BITS - 1);
  const int64_t one = (int64_t)1 << POOL_SHIFT_WEIGHT_BITS;
  return (Equity)(sum >= 0 ? (sum + half) / one : -((half - sum) / one));
}

// The subsets of a full rack in the WMP's order: counts of the highest
// letters vary fastest. Subsets i and count - 1 - i are complements.
// map_indices uses the generator's low bits for repeated kept letters.
typedef struct PoolShiftRack {
  int count;
  uint32_t ranks[1 << RACK_SIZE];
  MachineLetter bit_tiles[RACK_SIZE];
  int map_indices[1 << RACK_SIZE];
  uint8_t sizes[1 << RACK_SIZE];
  Equity shifts[1 << RACK_SIZE];
  // The tiles' weights (pool_shift_fill_weights), or with the exact leave
  // values the unseen tiles' counts: either way they change with the pool,
  // and with nothing else the shifts of a rack depend on.
  int32_t weights[POOL_SHIFT_MAX_STRIDE];
} PoolShiftRack;

// Prepare the weights, ranks and exact shifts once for a full mover rack.
void pool_shift_fill_rack(const PoolShiftTable *table,
                          const LetterDistribution *ld, const Bag *bag,
                          const Rack *opponent_rack, int weight_milli,
                          const Rack *rack, PoolShiftRack *result);

// The pool shift of keeping leave for a mover who holds tiles_on_rack tiles
// and sees the bag, of real_bag_size tiles, and opponent_rack as unseen: 0
// when it does not apply (pool_shift_applies) or the leave has no row. This
// is what move generation adds, for callers that value one move.
Equity pool_shift_get_leave_shift(const PoolShiftTable *table, const KLV *klv,
                                  int weight_milli,
                                  const LetterDistribution *ld, const Bag *bag,
                                  const Rack *opponent_rack, const Rack *leave,
                                  int tiles_on_rack, int real_bag_size);

#endif
