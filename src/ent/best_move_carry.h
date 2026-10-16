#ifndef BEST_MOVE_CARRY_H
#define BEST_MOVE_CARRY_H

#include "../def/board_defs.h"
#include "../def/move_defs.h"
#include "../util/io_util.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// A simulation worker's memory of the best moves its playouts found, by rack.
// The same rack recurs across a simulation's playouts (every candidate faces
// the same opponent racks, and bag cycling draws each rack again), on boards
// that differ only in a few lanes. A remembered move that is still legal on
// the new board is a move the generator would find there too, so its equity
// there is a floor on the best move's, and seeding the generator with it lets
// the generator skip every anchor that cannot beat it from the start.
//
// Each move also keeps a hash of every lane of the board it was best on. A
// lane that hashes the same now holds the same moves at the same equities as
// it did then (see gen_skip_unchanged_lanes), none better than that move.

enum {
  BEST_MOVE_CARRY_NUM_BUCKETS = 1024,
  BEST_MOVE_CARRY_MOVES_PER_RACK = 4,
};

typedef struct CarriedBestMove {
  Move move;
  // The letters on the board under the move's played-through squares, by
  // position along the move. Move.tiles marks them only as played through,
  // and on another board the same squares may hold other letters.
  MachineLetter board_letters[MOVE_MAX_TILES];
  // Whether lane_hashes and number_of_tiles_in_bag describe the board the
  // move was best on. False when that generation's equities read more of
  // the board than the move's own lane (PAT).
  bool has_lane_hashes;
  int number_of_tiles_in_bag;
  // By lane, BOARD_DIM * dir + row as in the generator's anchor counts.
  uint64_t lane_hashes[2 * BOARD_DIM];
} CarriedBestMove;

typedef struct BestMoveCarryBucket {
  uint64_t key;
  Rack rack;
  int player_index;
  int count;
  int next_slot;
  CarriedBestMove moves[BEST_MOVE_CARRY_MOVES_PER_RACK];
} BestMoveCarryBucket;

typedef struct BestMoveCarry {
  BestMoveCarryBucket buckets[BEST_MOVE_CARRY_NUM_BUCKETS];
  // The current generation's lane hashes, kept here so that only
  // generations that carry moves pay for them.
  uint64_t lane_hashes[2 * BOARD_DIM];
} BestMoveCarry;

static inline void best_move_carry_reset(BestMoveCarry *carry) {
  for (int bucket_idx = 0; bucket_idx < BEST_MOVE_CARRY_NUM_BUCKETS;
       bucket_idx++) {
    carry->buckets[bucket_idx].count = 0;
  }
}

static inline BestMoveCarry *best_move_carry_create(void) {
  BestMoveCarry *carry = (BestMoveCarry *)malloc_or_die(sizeof(BestMoveCarry));
  best_move_carry_reset(carry);
  return carry;
}

static inline void best_move_carry_destroy(BestMoveCarry *carry) {
  free(carry);
}

static inline uint64_t best_move_carry_get_key(const Rack *rack,
                                               int player_index) {
  uint64_t key = (uint64_t)player_index + 1;
  const uint16_t dist_size = rack_get_dist_size(rack);
  for (uint16_t ml = 0; ml < dist_size; ml++) {
    key = (key ^ rack_get_letter(rack, ml)) * UINT64_C(0x100000001B3);
  }
  return key;
}

static inline bool
best_move_carry_bucket_matches(const BestMoveCarryBucket *bucket, uint64_t key,
                               const Rack *rack, int player_index) {
  if (bucket->count == 0 || bucket->key != key ||
      bucket->player_index != player_index ||
      rack_get_dist_size(&bucket->rack) != rack_get_dist_size(rack)) {
    return false;
  }
  return memcmp(bucket->rack.array, rack->array,
                sizeof(rack->array[0]) * rack_get_dist_size(rack)) == 0;
}

// The bucket holding the moves remembered for this rack, or NULL.
static inline const BestMoveCarryBucket *
best_move_carry_find(const BestMoveCarry *carry, const Rack *rack,
                     int player_index) {
  const uint64_t key = best_move_carry_get_key(rack, player_index);
  const BestMoveCarryBucket *bucket =
      &carry->buckets[key & (BEST_MOVE_CARRY_NUM_BUCKETS - 1)];
  if (!best_move_carry_bucket_matches(bucket, key, rack, player_index)) {
    return NULL;
  }
  return bucket;
}

static inline bool best_move_carry_same_placement(const Move *move,
                                                  const Move *other) {
  return move_get_row_start(move) == move_get_row_start(other) &&
         move_get_col_start(move) == move_get_col_start(other) &&
         move_get_dir(move) == move_get_dir(other) &&
         move_get_tiles_length(move) == move_get_tiles_length(other) &&
         memcmp(move->tiles, other->tiles,
                sizeof(move->tiles[0]) * move_get_tiles_length(move)) == 0;
}

// Remembers a best move for this rack, replacing the rack's oldest move when
// it already has BEST_MOVE_CARRY_MOVES_PER_RACK of them, or the bucket's
// moves for another rack. Returns the slot to fill: the move's own slot when
// the rack already remembers its placement, so the newest board replaces the
// older one.
static inline CarriedBestMove *best_move_carry_add(BestMoveCarry *carry,
                                                   const Rack *rack,
                                                   int player_index,
                                                   const Move *move) {
  const uint64_t key = best_move_carry_get_key(rack, player_index);
  BestMoveCarryBucket *bucket =
      &carry->buckets[key & (BEST_MOVE_CARRY_NUM_BUCKETS - 1)];
  if (!best_move_carry_bucket_matches(bucket, key, rack, player_index)) {
    bucket->key = key;
    rack_copy(&bucket->rack, rack);
    bucket->player_index = player_index;
    bucket->count = 0;
    bucket->next_slot = 0;
  }
  for (int move_idx = 0; move_idx < bucket->count; move_idx++) {
    CarriedBestMove *remembered = &bucket->moves[move_idx];
    if (best_move_carry_same_placement(&remembered->move, move)) {
      move_copy(&remembered->move, move);
      return remembered;
    }
  }
  CarriedBestMove *slot = &bucket->moves[bucket->next_slot];
  bucket->next_slot = (bucket->next_slot + 1) % BEST_MOVE_CARRY_MOVES_PER_RACK;
  if (bucket->count < BEST_MOVE_CARRY_MOVES_PER_RACK) {
    bucket->count++;
  }
  move_copy(&slot->move, move);
  return slot;
}

#endif
