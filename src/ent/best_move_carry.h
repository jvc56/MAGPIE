#ifndef BEST_MOVE_CARRY_H
#define BEST_MOVE_CARRY_H

#include "../def/board_defs.h"
#include "../def/move_defs.h"
#include "../util/io_util.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// A simulation's memory of the best moves its playouts found, by rack. The
// same rack recurs across a simulation's playouts (every candidate faces the
// same opponent racks, and bag cycling draws each rack again), on boards that
// differ only in a few lanes. A remembered move that is still legal on the
// new board is a move the generator would find there too, so its equity
// there is a floor on the best move's, and seeding the generator with it lets
// the generator skip every anchor that cannot beat it from the start.
//
// Each move also keeps a hash of every lane of the board it was best on. A
// lane that hashes the same now holds the same moves at the same equities as
// it did then (see gen_get_unchanged_lanes), none better than that move.
//
// All of a simulation's worker threads share one carry, since a playout's
// matching playouts (the same draws for other candidates) run on any of them.
// Each bucket has its own lock, held only to copy moves in or out.

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

// The moves remembered for one rack.
typedef struct CarriedRackMoves {
  uint64_t key;
  Rack rack;
  int player_index;
  int count;
  int next_slot;
  CarriedBestMove moves[BEST_MOVE_CARRY_MOVES_PER_RACK];
} CarriedRackMoves;

typedef struct BestMoveCarryBucket {
  atomic_flag lock;
  CarriedRackMoves rack_moves;
} BestMoveCarryBucket;

typedef struct BestMoveCarry {
  BestMoveCarryBucket buckets[BEST_MOVE_CARRY_NUM_BUCKETS];
} BestMoveCarry;

// Not thread safe: call it while no generation uses the carry.
static inline void best_move_carry_reset(BestMoveCarry *carry) {
  for (int bucket_idx = 0; bucket_idx < BEST_MOVE_CARRY_NUM_BUCKETS;
       bucket_idx++) {
    atomic_flag_clear(&carry->buckets[bucket_idx].lock);
    carry->buckets[bucket_idx].rack_moves.count = 0;
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
best_move_carry_rack_moves_match(const CarriedRackMoves *rack_moves,
                                 uint64_t key, const Rack *rack,
                                 int player_index) {
  if (rack_moves->count == 0 || rack_moves->key != key ||
      rack_moves->player_index != player_index ||
      rack_get_dist_size(&rack_moves->rack) != rack_get_dist_size(rack)) {
    return false;
  }
  return memcmp(rack_moves->rack.array, rack->array,
                sizeof(rack->array[0]) * rack_get_dist_size(rack)) == 0;
}

// Locks and returns the bucket for key.
static inline BestMoveCarryBucket *best_move_carry_lock(BestMoveCarry *carry,
                                                        uint64_t key) {
  BestMoveCarryBucket *bucket =
      &carry->buckets[key & (BEST_MOVE_CARRY_NUM_BUCKETS - 1)];
  while (
      atomic_flag_test_and_set_explicit(&bucket->lock, memory_order_acquire)) {
    // The holder only copies a few hundred bytes; spin until it is done.
  }
  return bucket;
}

static inline void best_move_carry_unlock(BestMoveCarryBucket *bucket) {
  atomic_flag_clear_explicit(&bucket->lock, memory_order_release);
}

// Copies the moves remembered for this rack into rack_moves. Returns false,
// leaving rack_moves unspecified, when there are none.
static inline bool best_move_carry_find(BestMoveCarry *carry, const Rack *rack,
                                        int player_index,
                                        CarriedRackMoves *rack_moves) {
  const uint64_t key = best_move_carry_get_key(rack, player_index);
  BestMoveCarryBucket *bucket = best_move_carry_lock(carry, key);
  const bool found = best_move_carry_rack_moves_match(&bucket->rack_moves, key,
                                                      rack, player_index);
  if (found) {
    *rack_moves = bucket->rack_moves;
  }
  best_move_carry_unlock(bucket);
  return found;
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

// Remembers a best move for this rack. It replaces the rack's own entry for
// the same placement, so the newest board replaces the older one; otherwise
// the rack's oldest move when it already has BEST_MOVE_CARRY_MOVES_PER_RACK
// of them, or the bucket's moves for another rack.
static inline void best_move_carry_add(BestMoveCarry *carry, const Rack *rack,
                                       int player_index,
                                       const CarriedBestMove *carried) {
  const uint64_t key = best_move_carry_get_key(rack, player_index);
  BestMoveCarryBucket *bucket = best_move_carry_lock(carry, key);
  CarriedRackMoves *rack_moves = &bucket->rack_moves;
  if (!best_move_carry_rack_moves_match(rack_moves, key, rack, player_index)) {
    rack_moves->key = key;
    rack_copy(&rack_moves->rack, rack);
    rack_moves->player_index = player_index;
    rack_moves->count = 0;
    rack_moves->next_slot = 0;
  }
  CarriedBestMove *slot = NULL;
  for (int move_idx = 0; move_idx < rack_moves->count; move_idx++) {
    if (best_move_carry_same_placement(&rack_moves->moves[move_idx].move,
                                       &carried->move)) {
      slot = &rack_moves->moves[move_idx];
      break;
    }
  }
  if (slot == NULL) {
    slot = &rack_moves->moves[rack_moves->next_slot];
    rack_moves->next_slot =
        (rack_moves->next_slot + 1) % BEST_MOVE_CARRY_MOVES_PER_RACK;
    if (rack_moves->count < BEST_MOVE_CARRY_MOVES_PER_RACK) {
      rack_moves->count++;
    }
  }
  *slot = *carried;
  best_move_carry_unlock(bucket);
}

#endif
