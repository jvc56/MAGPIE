#ifndef KWG_H
#define KWG_H

#include <stdbool.h>
#include <stdint.h>

struct KWG;
typedef struct KWG KWG;

KWG *create_kwg(const char *kwg_name);
void destroy_kwg(KWG *kwg);
// FIXME: find a way to force inline these functions
// with an opaque pointer.
inline bool kwg_is_end(const KWG *kwg, int node_index);
inline bool kwg_accepts(const KWG *kwg, int node_index);
inline int kwg_arc_index(const KWG *kwg, int node_index);
inline int kwg_tile(const KWG *kwg, int node_index);
inline int kwg_get_root_node_index(const KWG *kwg);
int kwg_get_next_node_index(const KWG *kwg, int node_index, int letter);
bool kwg_in_letter_set(const KWG *kwg, int letter, int node_index);
uint64_t kwg_get_letter_set(const KWG *kwg, int node_index);

#endif