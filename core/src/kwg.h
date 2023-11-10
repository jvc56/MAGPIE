#ifndef KWG_H
#define KWG_H

#include <stdint.h>

typedef struct KWG {
  uint32_t *nodes;
} KWG;

KWG *create_kwg(const char *kwg_name);
void destroy_kwg(KWG *kwg);

inline int kwg_is_end(const KWG *kwg, int node_index) {
  return (kwg->nodes[node_index] & 0x400000) != 0;
}

inline int kwg_accepts(const KWG *kwg, int node_index) {
  return (kwg->nodes[node_index] & 0x800000) != 0;
}

inline int kwg_arc_index(const KWG *kwg, int node_index) {
  return (kwg->nodes[node_index] & 0x3fffff);
}

inline int kwg_tile(const KWG *kwg, int node_index) {
  return kwg->nodes[node_index] >> 24;
}

inline int kwg_get_root_node_index(const KWG *kwg) {
  return kwg_arc_index(kwg, 1);
}
int kwg_get_next_node_index(const KWG *kwg, int node_index, int letter);
int kwg_in_letter_set(const KWG *kwg, int letter, int node_index);
uint64_t kwg_get_letter_set(const KWG *kwg, int node_index);

#endif