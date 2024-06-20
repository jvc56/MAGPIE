#ifndef KWG_H
#define KWG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../def/kwg_defs.h"

typedef struct KWG {
  char *name;
  uint32_t *nodes;
  int number_of_nodes;
} KWG;

KWG *kwg_create(const char *data_dir, const char *kwg_name);
KWG *kwg_create_empty();
bool kwg_write_to_file(const KWG *kwg, const char *filename);
void kwg_destroy(KWG *kwg);
void kwg_read_nodes_from_stream(KWG *kwg, size_t number_of_nodes, FILE *stream);
void kwg_allocate_nodes(KWG *kwg, size_t number_of_nodes);
uint32_t *kwg_get_mutable_nodes(KWG *kwg);

static inline const char *kwg_get_name(const KWG *kwg) { return kwg->name; }

static inline uint32_t kwg_node(const KWG *kwg, uint32_t node_index) {
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

static inline uint32_t kwg_node_arc_index_prefetch(uint32_t node,
                                                   const KWG *kwg) {
  const uint32_t next_node = (node & KWG_ARC_INDEX_MASK);
#ifdef __has_builtin
#if __has_builtin(__builtin_prefetch)
  __builtin_prefetch(&kwg->nodes[next_node]);
#endif
#endif
  return next_node;
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

static inline uint32_t
kwg_get_next_node_index(const KWG *kwg, uint32_t node_index, uint8_t letter) {
  uint32_t i = node_index;
  while (1) {
    const uint32_t node = kwg_node(kwg, i);
    if (kwg_node_tile(node) == letter) {
      return kwg_node_arc_index_prefetch(node, kwg);
    }
    if (kwg_node_is_end(node)) {
      return 0;
    }
    i++;
  }
}

bool kwg_in_letter_set(const KWG *kwg, uint8_t letter, uint32_t node_index);

static inline uint64_t kwg_get_letter_sets(const KWG *kwg, uint32_t node_index,
                                           uint64_t *extension_set) {
  uint64_t ls = 0, es = 0;
  for (uint32_t i = node_index;; ++i) {
    const uint32_t node = kwg_node(kwg, i);
    const uint32_t t = kwg_node_tile(node);
    const uint64_t bit = ((uint64_t)1 << t) ^ !t;
    es |= bit;
    ls |= bit & (uint64_t) - (int64_t)kwg_node_accepts(node);
    if (kwg_node_is_end(node)) {
      break;
    }
  }
  *extension_set = es;
  return ls;
}

int kwg_get_number_of_nodes(const KWG *kwg);

#endif