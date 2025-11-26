#include "rack_hash_table.h"
#include "../compat/malloc.h"

RackHashTable *rack_hash_table_create(size_t num_buckets, int move_list_capacity,
                                      size_t num_stripes) {
  RackHashTable *rht = (RackHashTable *)malloc_or_die(sizeof(RackHashTable));
  rht->num_buckets = num_buckets;
  rht->move_list_capacity = move_list_capacity;
  rht->num_stripes = num_stripes;

  // Ensure num_buckets is power of 2 for bit_rack_get_bucket_index
  // (Assuming the caller provides a power of 2 or we strictly require it.
  // bit_rack_get_bucket_index requires it.)
  // Let's enforce/check it or just assume it for now.

  rht->buckets = (InferredRackMoveList **)calloc_or_die(
      num_buckets, sizeof(InferredRackMoveList *));

  rht->locks =
      (cpthread_mutex_t *)malloc_or_die(num_stripes * sizeof(cpthread_mutex_t));
  for (size_t i = 0; i < num_stripes; i++) {
    cpthread_mutex_init(&rht->locks[i]);
  }

  return rht;
}

void rack_hash_table_destroy(RackHashTable *rht) {
  if (!rht)
    return;

  for (size_t i = 0; i < rht->num_buckets; i++) {
    InferredRackMoveList *node = rht->buckets[i];
    while (node) {
      InferredRackMoveList *next = node->next;
      move_list_destroy(node->moves);
      free(node);
      node = next;
    }
  }
  free(rht->buckets);

  for (size_t i = 0; i < rht->num_stripes; i++) {
    cpthread_mutex_destroy(&rht->locks[i]);
  }
  free(rht->locks);
  free(rht);
}

void rack_hash_table_add_move(RackHashTable *rht, const BitRack *rack,
                              Equity leave_value, int draws, float weight,
                              const Move *move) {
  uint32_t bucket_idx = bit_rack_get_bucket_index(rack, rht->num_buckets);
  // bit_rack_mix_to_64 is used inside bit_rack_get_bucket_index.
  // We can use the raw hash to determine the stripe.
  // Or just map bucket_idx to stripe.
  // Stripe index = bucket_idx % num_stripes?
  // Ideally num_buckets is much larger than num_stripes.
  // If num_buckets is power of 2, and num_stripes is power of 2, this works
  // well.
  size_t stripe_idx = bucket_idx % rht->num_stripes;

  cpthread_mutex_lock(&rht->locks[stripe_idx]);

  InferredRackMoveList *node = rht->buckets[bucket_idx];
  while (node) {
    if (bit_rack_equals(&node->rack, rack)) {
      break;
    }
    node = node->next;
  }

  if (!node) {
    node = (InferredRackMoveList *)malloc_or_die(sizeof(InferredRackMoveList));
    node->rack = *rack;
    node->leave_value = leave_value;
    node->draws = draws;
    node->weight = weight;
    node->moves = move_list_create(rht->move_list_capacity);
    node->next = rht->buckets[bucket_idx];
    rht->buckets[bucket_idx] = node;
  }

  // Update properties if needed (Should be constant for the same rack)
  node->leave_value = leave_value;
  node->draws = draws;
  node->weight = weight;

  MoveList *ml = node->moves;

  // Logic to insert move into MoveList (Heap)
  // If not full, insert.
  // If full, compare with min (ml->moves[0]), if better, pop min and insert.
  // Note: MoveList is a min-heap of size K. The smallest element is at 0.
  // We want to keep the K largest elements.

  // Using move_list_insert_spare_move assumes we populate spare_move.
  // We need to check if we SHOULD insert before copying, to save time?
  // The check is: if count < capacity OR move > moves[0].

  bool should_insert = false;
  if (ml->count < ml->capacity) {
    should_insert = true;
  } else {
    // Compare input move with worst move in heap (moves[0])
    // We want to keep TOP moves. So if input move is better than worst, we
    // insert. compare_moves(a, b) returns 1 if a > b. We want input > moves[0].
    if (compare_moves(move, ml->moves[0], false) == 1) {
      should_insert = true;
    }
  }

  if (should_insert) {
    Move *spare = move_list_get_spare_move(ml);
    move_copy(spare, move);
    move_list_insert_spare_move(ml, move->equity);
  }

  cpthread_mutex_unlock(&rht->locks[stripe_idx]);
}

InferredRackMoveList *rack_hash_table_lookup(RackHashTable *rht,
                                             const BitRack *rack) {
  uint32_t bucket_idx = bit_rack_get_bucket_index(rack, rht->num_buckets);
  // We technically don't need lock if we are in a read-only phase.
  // But to be safe/correct with API:
  size_t stripe_idx = bucket_idx % rht->num_stripes;
  cpthread_mutex_lock(&rht->locks[stripe_idx]);

  InferredRackMoveList *node = rht->buckets[bucket_idx];
  while (node) {
    if (bit_rack_equals(&node->rack, rack)) {
      break;
    }
    node = node->next;
  }
  // CAUTION: returning pointer to node inside locked structure.
  // If caller modifies it or reads while another thread writes, it's a race.
  // BUT, the intended usage is:
  // 1. Generation phase (Many writers using add_move).
  // 2. Inference phase (Many readers using lookup).
  // So purely read access during phase 2 is fine without locks if no writes happen.
  // However, standard locking requires unlock.
  cpthread_mutex_unlock(&rht->locks[stripe_idx]);

  return node;
}