#ifndef KWG_H
#define KWG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct KWG {
  uint32_t *nodes;
} KWG;

KWG *create_kwg(const char *kwg_name);
void destroy_kwg(KWG *kwg);

inline uint32_t kwg_node(const KWG *kwg, int node_index) {
  return kwg->nodes[node_index];
}

inline bool kwg_is_end(const KWG *kwg, int node_index) {
  return (kwg->nodes[node_index] & 0x400000) != 0;
}

inline bool kwg_node_is_end(uint32_t node) {
  return (node & 0x400000) != 0;
}

inline bool kwg_accepts(const KWG *kwg, int node_index) {
  return (kwg->nodes[node_index] & 0x800000) != 0;
}

inline bool kwg_node_accepts(uint32_t node) {
  return (node & 0x800000) != 0;
}

inline int kwg_arc_index(const KWG *kwg, int node_index) {
  return (kwg->nodes[node_index] & 0x3fffff);
}

inline int kwg_node_arc_index(uint32_t node) {
  return (node & 0x3fffff);
}

inline int kwg_tile(const KWG *kwg, int node_index) {
  return kwg->nodes[node_index] >> 24;
}

inline int kwg_node_tile(uint32_t node) {
  return node >> 24;
}

inline int kwg_get_root_node_index(const KWG *kwg) {
  return kwg_arc_index(kwg, 1);
}
int kwg_get_next_node_index(const KWG *kwg, int node_index, int letter);
bool kwg_in_letter_set(const KWG *kwg, int letter, int node_index);
uint64_t kwg_get_letter_set(const KWG *kwg, int node_index);

#endif