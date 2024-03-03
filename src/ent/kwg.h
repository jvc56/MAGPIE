#ifndef KWG_H
#define KWG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../def/kwg_defs.h"

typedef struct KWG {
  uint32_t *nodes;
  int number_of_nodes;
} KWG;

KWG *kwg_create(const char *kwg_name);
KWG *kwg_create_empty();
void kwg_destroy(KWG *kwg);
void kwg_read_nodes_from_stream(KWG *kwg, size_t number_of_nodes, FILE *stream);
void kwg_allocate_nodes(KWG *kwg, size_t number_of_nodes);
uint32_t *kwg_get_mutable_nodes(KWG *kwg);

static inline uint32_t kwg_node(const KWG *kwg, int node_index) {
  return kwg->nodes[node_index];
}

static inline bool kwg_node_is_end(uint32_t node) {
  return (node & KWG_NODE_IS_END_FLAG) != 0;
}

static inline bool kwg_node_accepts(uint32_t node) {
  return (node & KWG_NODE_ACCEPTS_FLAG) != 0;
}

static inline uint32_t kwg_node_arc_index(uint32_t node) {
  return (node & KWG_ARC_INDEX_MASK);
}

static inline uint32_t kwg_node_tile(uint32_t node) {
  return node >> KWG_TILE_BIT_OFFSET;
}

static inline uint32_t kwg_get_dawg_root_node_index(const KWG *kwg) {
  const uint32_t dawg_pointer_node = kwg_node(kwg, 0);
  return kwg_node_arc_index(dawg_pointer_node);
}

static inline uint32_t kwg_get_root_node_index(const KWG *kwg) {
  const uint32_t gaddag_pointer_node = kwg_node(kwg, 1);
  return kwg_node_arc_index(gaddag_pointer_node);
}

static inline uint32_t kwg_get_next_node_index(const KWG *kwg, int node_index,
                                               uint8_t letter) {
  int i = node_index;
  while (1) {
    const uint32_t node = kwg_node(kwg, i);
    if (kwg_node_tile(node) == letter) {
      return kwg_node_arc_index(node);
    }
    if (kwg_node_is_end(node)) {
      return 0;
    }
    i++;
  }
}

bool kwg_in_letter_set(const KWG *kwg, uint8_t letter, uint32_t node_index);
uint64_t kwg_get_letter_set(const KWG *kwg, uint32_t node_index);
int kwg_get_number_of_nodes(const KWG *kwg);

#endif