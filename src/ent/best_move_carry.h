#ifndef BEST_MOVE_CARRY_H
#define BEST_MOVE_CARRY_H

#include "../def/board_defs.h"
#include "../def/move_defs.h"
#include "../def/static_eval_defs.h"
#include "../util/io_util.h"
#include "board.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
#include <assert.h>
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
// A carry created to skip lanes also remembers, for each lane, bounds by the
// lane's hash: the best move of a generation that searched the lane while it
// hashed so. A lane that hashes the same now holds the same moves at the
// same equities as it did then (see gen_get_skippable_lanes), none better
// than that bound. When the bound is itself a move of the lane it is the
// lane's best move, and still legal. The bounds take about 11 KB a bucket,
// nearly all of the carry's memory; a simulation sizes the carry by
// SIM_BEST_MOVE_CARRY_BUCKETS (see memory_info.h).
//
// All of a simulation's worker threads share one carry, since a playout's
// matching playouts (the same draws for other candidates) run on any of them.
// Each bucket has its own lock, held only to copy moves in or out.

enum {
  BEST_MOVE_CARRY_MOVES_PER_RACK = 4,
  BEST_MOVE_CARRY_NUM_LANES = 2 * BOARD_DIM,
  BEST_MOVE_CARRY_BOUNDS_PER_LANE = 8,
};

static_assert(BEST_MOVE_CARRY_NUM_LANES <= 64, "lane masks are 64 bits");

typedef struct CarriedBestMove {
  Move move;
  // The letters on the board under the move's played-through squares, by
  // position along the move. Move.tiles marks them only as played through,
  // and on another board the same squares may hold other letters.
  MachineLetter board_letters[MOVE_MAX_TILES];
} CarriedBestMove;

// No move of a lane hashing lane_hash beats bound, given a bag that gives
// the same equities as number_of_tiles_in_bag. A lane_hash of 0 is unused.
typedef struct CarriedLaneBound {
  uint64_t lane_hash;
  int number_of_tiles_in_bag;
  // Whether bound is a play in this lane, and so the lane's best move.
  bool bound_is_in_lane;
  Move bound;
} CarriedLaneBound;

// The moves remembered for one rack.
typedef struct CarriedRackMoves {
  bool is_valid;
  uint64_t key;
  Rack rack;
  int player_index;
  int count;
  int next_slot;
  CarriedBestMove moves[BEST_MOVE_CARRY_MOVES_PER_RACK];
} CarriedRackMoves;

// The lane bounds remembered for the rack in the same bucket.
typedef struct CarriedLaneBounds {
  CarriedLaneBound bounds[BEST_MOVE_CARRY_NUM_LANES]
                         [BEST_MOVE_CARRY_BOUNDS_PER_LANE];
  uint8_t next_slot[BEST_MOVE_CARRY_NUM_LANES];
} CarriedLaneBounds;

// What a generation reads from the carry: the rack's moves, and a bound for
// each lane in lanes_with_bounds.
typedef struct CarriedRackFind {
  int count;
  CarriedBestMove moves[BEST_MOVE_CARRY_MOVES_PER_RACK];
  uint64_t lanes_with_bounds;
  CarriedLaneBound lane_bounds[BEST_MOVE_CARRY_NUM_LANES];
} CarriedRackFind;

typedef struct BestMoveCarryBucket {
  atomic_flag lock;
  CarriedRackMoves rack_moves;
} BestMoveCarryBucket;

typedef struct BestMoveCarry {
  // A power of two, so that a key's low bits pick its bucket.
  int num_buckets;
  BestMoveCarryBucket *buckets;
  // By bucket, under the bucket's lock; NULL when the carry skips no lanes.
  CarriedLaneBounds *lane_bounds;
} BestMoveCarry;

// Whether every move gets the same equity from either bag: the bag's only
// term is peg_adjust_values, which no play from a bag that full reaches.
static inline bool best_move_carry_bags_give_same_equities(int tiles_in_bag,
                                                           int other_in_bag) {
  return tiles_in_bag == other_in_bag ||
         (tiles_in_bag >= PEG_ADJUST_VALUES_LENGTH &&
          other_in_bag >= PEG_ADJUST_VALUES_LENGTH);
}

// Not thread safe: call it while no generation uses the carry.
static inline void best_move_carry_reset(BestMoveCarry *carry) {
  for (int bucket_idx = 0; bucket_idx < carry->num_buckets; bucket_idx++) {
    atomic_flag_clear(&carry->buckets[bucket_idx].lock);
    carry->buckets[bucket_idx].rack_moves.is_valid = false;
  }
}

// num_buckets must be a power of two.
static inline BestMoveCarry *best_move_carry_create(int num_buckets,
                                                    bool skips_lanes) {
  assert(num_buckets > 0 && (num_buckets & (num_buckets - 1)) == 0);
  BestMoveCarry *carry = (BestMoveCarry *)malloc_or_die(sizeof(BestMoveCarry));
  carry->num_buckets = num_buckets;
  carry->buckets = (BestMoveCarryBucket *)malloc_or_die(
      sizeof(BestMoveCarryBucket) * (size_t)num_buckets);
  carry->lane_bounds = NULL;
  if (skips_lanes) {
    carry->lane_bounds = (CarriedLaneBounds *)malloc_or_die(
        sizeof(CarriedLaneBounds) * (size_t)num_buckets);
  }
  best_move_carry_reset(carry);
  return carry;
}

static inline void best_move_carry_destroy(BestMoveCarry *carry) {
  if (carry == NULL) {
    return;
  }
  free(carry->lane_bounds);
  free(carry->buckets);
  free(carry);
}

static inline bool best_move_carry_skips_lanes(const BestMoveCarry *carry) {
  return carry->lane_bounds != NULL;
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
  if (!rack_moves->is_valid || rack_moves->key != key ||
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
      &carry->buckets[key & (uint64_t)(carry->num_buckets - 1)];
  while (
      atomic_flag_test_and_set_explicit(&bucket->lock, memory_order_acquire)) {
    // The holder only copies a few hundred bytes; spin until it is done.
  }
  return bucket;
}

static inline void best_move_carry_unlock(BestMoveCarryBucket *bucket) {
  atomic_flag_clear_explicit(&bucket->lock, memory_order_release);
}

// Copies the moves remembered for this rack into found, with a bound for
// each lane whose hash in lane_hashes (by lane, BOARD_DIM * dir + row as in
// the generator's anchor counts) has one for this bag. A NULL lane_hashes
// reads no bounds; a carry that skips no lanes needs NULL. Returns false,
// leaving found unspecified, when nothing is remembered for the rack.
static inline bool best_move_carry_find(BestMoveCarry *carry, const Rack *rack,
                                        int player_index,
                                        const uint64_t *lane_hashes,
                                        int number_of_tiles_in_bag,
                                        CarriedRackFind *found) {
  const uint64_t key = best_move_carry_get_key(rack, player_index);
  const int bucket_idx = (int)(key & (uint64_t)(carry->num_buckets - 1));
  BestMoveCarryBucket *bucket = best_move_carry_lock(carry, key);
  const CarriedRackMoves *rack_moves = &bucket->rack_moves;
  const bool is_found =
      best_move_carry_rack_moves_match(rack_moves, key, rack, player_index);
  if (is_found) {
    found->count = rack_moves->count;
    memcpy(found->moves, rack_moves->moves,
           sizeof(found->moves[0]) * (size_t)rack_moves->count);
    found->lanes_with_bounds = 0;
    for (int lane = 0; lane_hashes != NULL && lane < BEST_MOVE_CARRY_NUM_LANES;
         lane++) {
      if (lane_hashes[lane] == 0) {
        continue;
      }
      for (int bound_idx = 0; bound_idx < BEST_MOVE_CARRY_BOUNDS_PER_LANE;
           bound_idx++) {
        const CarriedLaneBound *lane_bound =
            &carry->lane_bounds[bucket_idx].bounds[lane][bound_idx];
        if (lane_bound->lane_hash == lane_hashes[lane] &&
            best_move_carry_bags_give_same_equities(
                lane_bound->number_of_tiles_in_bag, number_of_tiles_in_bag)) {
          found->lane_bounds[lane] = *lane_bound;
          found->lanes_with_bounds |= (uint64_t)1 << lane;
          break;
        }
      }
    }
  }
  best_move_carry_unlock(bucket);
  return is_found;
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

static inline void best_move_carry_add_move(CarriedRackMoves *rack_moves,
                                            const CarriedBestMove *carried) {
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
}

// The lane a tile placement is played in, BOARD_DIM * dir + row.
static inline int best_move_carry_get_move_lane(const Move *move) {
  const int dir = move_get_dir(move);
  return BOARD_DIM * dir + (board_is_dir_vertical(dir)
                                ? move_get_col_start(move)
                                : move_get_row_start(move));
}

// Bounds lane by best_move, keeping the lane's lower bound for its hash.
static inline void
best_move_carry_add_lane_bound(CarriedLaneBounds *rack_lane_bounds, int lane,
                               uint64_t lane_hash, int number_of_tiles_in_bag,
                               const Move *best_move) {
  CarriedLaneBound *lane_bounds = rack_lane_bounds->bounds[lane];
  CarriedLaneBound *slot = NULL;
  for (int bound_idx = 0; bound_idx < BEST_MOVE_CARRY_BOUNDS_PER_LANE;
       bound_idx++) {
    CarriedLaneBound *lane_bound = &lane_bounds[bound_idx];
    if (lane_bound->lane_hash == lane_hash &&
        best_move_carry_bags_give_same_equities(
            lane_bound->number_of_tiles_in_bag, number_of_tiles_in_bag)) {
      if (compare_moves(best_move, &lane_bound->bound, true) != 0) {
        return;
      }
      slot = lane_bound;
      break;
    }
  }
  if (slot == NULL) {
    slot = &lane_bounds[rack_lane_bounds->next_slot[lane]];
    rack_lane_bounds->next_slot[lane] =
        (uint8_t)((rack_lane_bounds->next_slot[lane] + 1) %
                  BEST_MOVE_CARRY_BOUNDS_PER_LANE);
  }
  slot->lane_hash = lane_hash;
  slot->number_of_tiles_in_bag = number_of_tiles_in_bag;
  slot->bound_is_in_lane =
      move_get_type(best_move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
      best_move_carry_get_move_lane(best_move) == lane;
  slot->bound = *best_move;
}

// Remembers a generation's best move for this rack: carried, if not NULL,
// as a move to try on later boards, and, in a carry that skips lanes,
// best_move as a bound on each lane in searched_lanes, whose hashes are in
// lane_hashes. A carried move replaces
// the rack's own entry for the same placement, so the newest board replaces
// the older one; otherwise the rack's oldest move when it already has
// BEST_MOVE_CARRY_MOVES_PER_RACK of them, or the bucket's moves for another
// rack.
static inline void
best_move_carry_add(BestMoveCarry *carry, const Rack *rack, int player_index,
                    const CarriedBestMove *carried, const Move *best_move,
                    const uint64_t *lane_hashes, uint64_t searched_lanes,
                    int number_of_tiles_in_bag) {
  const uint64_t key = best_move_carry_get_key(rack, player_index);
  const int bucket_idx = (int)(key & (uint64_t)(carry->num_buckets - 1));
  BestMoveCarryBucket *bucket = best_move_carry_lock(carry, key);
  CarriedRackMoves *rack_moves = &bucket->rack_moves;
  CarriedLaneBounds *rack_lane_bounds = NULL;
  if (carry->lane_bounds != NULL) {
    rack_lane_bounds = &carry->lane_bounds[bucket_idx];
  }
  if (!best_move_carry_rack_moves_match(rack_moves, key, rack, player_index)) {
    rack_moves->is_valid = true;
    rack_moves->key = key;
    rack_copy(&rack_moves->rack, rack);
    rack_moves->player_index = player_index;
    rack_moves->count = 0;
    rack_moves->next_slot = 0;
    for (int lane = 0;
         rack_lane_bounds != NULL && lane < BEST_MOVE_CARRY_NUM_LANES; lane++) {
      for (int bound_idx = 0; bound_idx < BEST_MOVE_CARRY_BOUNDS_PER_LANE;
           bound_idx++) {
        rack_lane_bounds->bounds[lane][bound_idx].lane_hash = 0;
      }
      rack_lane_bounds->next_slot[lane] = 0;
    }
  }
  if (carried != NULL) {
    best_move_carry_add_move(rack_moves, carried);
  }
  for (int lane = 0;
       rack_lane_bounds != NULL && lane < BEST_MOVE_CARRY_NUM_LANES; lane++) {
    if (searched_lanes & ((uint64_t)1 << lane)) {
      best_move_carry_add_lane_bound(rack_lane_bounds, lane, lane_hashes[lane],
                                     number_of_tiles_in_bag, best_move);
    }
  }
  best_move_carry_unlock(bucket);
}

#endif
