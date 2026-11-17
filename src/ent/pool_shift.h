#ifndef POOL_SHIFT_H
#define POOL_SHIFT_H

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "bag.h"
#include "equity.h"
#include "klv.h"
#include "letter_distribution.h"
#include "rack.h"
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
// the combinatorial number system, where the leave whose tiles in ascending
// order are a_0 <= a_1 <= ... is number
//
//   sum over i of C(a_i + i, i + 1)
//
// of its size. That numbers every multiset of the alphabet, so the leaves
// with more of a tile than the distribution has get rows too, which nothing
// reads.

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
};

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
  // By leave size, the row of the first leave of that size.
  uint32_t size_starts[POOL_SHIFT_MAX_LEAVE_SIZE + 1];
  // rank_terms[i][t] is C(t + i, i + 1): what tile t adds to the number of a
  // leave with i tiles before it in ascending order.
  uint32_t rank_terms[POOL_SHIFT_MAX_LEAVE_SIZE][MAX_ALPHABET_SIZE];
  // number_of_rows rows of stride values: values[row * stride + t] is the
  // KLV's value of the row's leave with one more t.
  Equity *values;
} PoolShiftTable;

PoolShiftTable *pool_shift_table_create(const KLV *klv,
                                        const LetterDistribution *ld);
void pool_shift_table_destroy(PoolShiftTable *table);

// Whether the table was built from this KLV as it is now.
static inline bool pool_shift_table_matches_klv(const PoolShiftTable *table,
                                                const KLV *klv) {
  return klv != NULL && table->klv_instance_id == klv_get_instance_id(klv) &&
         table->klv_mutation_counter == klv_get_mutation_counter(klv);
}

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
         real_bag_size >= RACK_SIZE && pool_shift_table_matches_klv(table, klv);
}

// The number among the leaves of its size (see above) of a leave with one
// more tile: of the leave of leave_size tiles numbered rank with ml added,
// where no tile of the leave is greater than ml. Unchanged for a leave too
// long to have a row.
static inline uint32_t pool_shift_table_extend_rank(const PoolShiftTable *table,
                                                    uint32_t rank,
                                                    int leave_size,
                                                    MachineLetter ml) {
  if (leave_size >= POOL_SHIFT_MAX_LEAVE_SIZE) {
    return rank;
  }
  return rank + table->rank_terms[leave_size][ml];
}

// The row of the leave of leave_size tiles numbered rank among the leaves of
// its size.
static inline uint32_t pool_shift_table_get_row(const PoolShiftTable *table,
                                                uint32_t rank, int leave_size) {
  if (leave_size > POOL_SHIFT_MAX_LEAVE_SIZE) {
    return POOL_SHIFT_NO_ROW;
  }
  return table->size_starts[leave_size] + rank;
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
  __builtin_prefetch(values + 16);
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
