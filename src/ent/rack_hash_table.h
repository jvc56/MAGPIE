#ifndef RACK_HASH_TABLE_H
#define RACK_HASH_TABLE_H

#include "../compat/cpthread.h"
#include "bit_rack.h"
#include "equity.h"
#include "move.h"

typedef struct InferredRackMoveList {
  BitRack rack;
  Equity leave_value;
  MoveList *moves;
  struct InferredRackMoveList *next;
  int draws;
  float weight;
} InferredRackMoveList;

typedef struct RackHashTable {
  size_t num_buckets;
  size_t num_stripes;
  int move_list_capacity;
  cpthread_mutex_t *locks;
  InferredRackMoveList **buckets;
} RackHashTable;

RackHashTable *rack_hash_table_create(size_t num_buckets, int move_list_capacity,
                                      size_t num_stripes);
void rack_hash_table_destroy(RackHashTable *rht);

// Thread-safe insertion/update
void rack_hash_table_add_move(RackHashTable *rht, const BitRack *rack,
                              Equity leave_value, int draws, float weight,
                              const Move *move);

// Thread-safe retrieval (returns a copy of the list, or we expose a way to
// iterate) For now, let's just return the pointer. Caller must ensure safety or
// we return a copy? The RFC says "main thread iterates... performs O(1) lookup".
// Since generation and inference are separate phases (barrier in between), we
// might not need locks for reading if we are sure writing is done. But for now,
// let's just allow lookup.
InferredRackMoveList *rack_hash_table_lookup(RackHashTable *rht,
                                             const BitRack *rack);

#endif // RACK_HASH_TABLE_H