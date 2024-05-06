#include <assert.h>

#include "../../src/ent/config.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/kwg_cache.h"

#include "test_util.h"

void test_kwg_cache() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  KWG *kwg = players_data_get_kwg(config_get_players_data(config), 0);

  KWGCache kwgc;
  kwgc_init(&kwgc, kwg);

  // Assert node index of 0 is not cached:
  uint32_t kwg_node_0 = kwg_node(kwg, 0);
  assert(kwg_node_0 == kwgc_node(&kwgc, 0));
  uint32_t new_kwg_node_0_val = 5;
  kwg->nodes[0] = new_kwg_node_0_val;
  assert(new_kwg_node_0_val == kwgc_node(&kwgc, 0));
  kwg->nodes[0] = kwg_node_0;

  const uint32_t kwgc_index = 10;
  uint32_t colliding_node_indexes[KWG_CACHE_BUCKET_SIZE + 1];
  uint32_t colliding_node_values[KWG_CACHE_BUCKET_SIZE + 1];
  int collisions = 0;
  const uint32_t number_of_nodes = kwg_get_number_of_nodes(kwg);
  for (uint32_t i = 0; i < number_of_nodes; i++) {
    if (kwgc_hash(i) == kwgc_index) {
      colliding_node_indexes[collisions] = i;
      colliding_node_values[collisions] = kwg_node(kwg, i);
      collisions++;
      if (collisions == KWG_CACHE_BUCKET_SIZE + 1) {
        break;
      }
    }
  }

  assert(collisions == KWG_CACHE_BUCKET_SIZE + 1);

  // Ensure that there are KWG_CACHE_BUCKET_SIZE colliding indexes
  // loaded in the cache.
  for (int i = 0; i < KWG_CACHE_BUCKET_SIZE; i++) {
    const int colliding_index = i % KWG_CACHE_BUCKET_SIZE;
    const uint32_t node_index = colliding_node_indexes[colliding_index];
    kwgc_node(&kwgc, node_index);
  }

  // Continuously get colliding node index values that all fit
  // in the bucket. Since the bucket can contain all of the values,
  // nothing should be evicted and the underlying kwg_node should
  // never be called. The original node values should remain unchanged
  // in the kwgc.
  for (int i = 0; i < 1000; i++) {
    const int colliding_index = i % KWG_CACHE_BUCKET_SIZE;
    const uint32_t node_index = colliding_node_indexes[colliding_index];
    kwg->nodes[node_index] = i + 10;
    assert(colliding_node_values[colliding_index] ==
           kwgc_node(&kwgc, node_index));
  }

  kwgc_clear(&kwgc);

  for (int i = 0; i < KWG_CACHE_BUCKET_SIZE + 1; i++) {
    const int colliding_index = i;
    const uint32_t node_index = colliding_node_indexes[colliding_index];
    const uint32_t new_node_value = i + 10;
    kwg->nodes[node_index] = new_node_value;
    assert(new_node_value == kwgc_node(&kwgc, node_index));
  }

  kwgc_clear(&kwgc);

  // Continuously get colliding node index values that don't fit
  // in the bucket, resulting in contiuous misses since values
  // are replaced on an LRU basis.
  for (int i = 0; i < 1000; i++) {
    const int colliding_index = i % (KWG_CACHE_BUCKET_SIZE + 1);
    const uint32_t node_index = colliding_node_indexes[colliding_index];
    const uint32_t new_node_value = i + 10000;
    kwg->nodes[node_index] = new_node_value;
    assert(new_node_value == kwgc_node(&kwgc, node_index));
  }

  config_destroy(config);
}