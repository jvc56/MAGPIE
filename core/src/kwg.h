#ifndef KWG_H
#define KWG_H

#include <stdint.h>

typedef struct KWG {
  uint32_t *nodes;
} KWG;

KWG *create_kwg(const char *kwg_filename);
void destroy_kwg(KWG *kwg);

inline int kwg_is_end(KWG *kwg, int32_t node_index) {
  return (kwg->nodes[node_index] & 0x400000) != 0;
}

inline int kwg_accepts(KWG *kwg, int32_t node_index) {
  return (kwg->nodes[node_index] & 0x800000) != 0;
}

inline int32_t kwg_arc_index(KWG *kwg, int32_t node_index) {
  return (kwg->nodes[node_index] & 0x3fffff);
}

inline int kwg_tile(KWG *kwg, int32_t node_index) {
  return kwg->nodes[node_index] >> 24;
}

inline int32_t kwg_get_root_node_index(KWG *kwg) {
  return kwg_arc_index(kwg, 1);
}
int32_t kwg_get_next_node_index(KWG *kwg, int32_t node_index, int letter);
int kwg_in_letter_set(KWG *kwg, int letter, int32_t node_index);
int kwg_get_letter_set(KWG *kwg, int32_t node_index);

#endif