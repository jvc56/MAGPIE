#ifndef KWG_H
#define KWG_H

#include <stdint.h>

#include "alphabet.h"

typedef struct KWG {
    uint32_t * nodes;
    Alphabet * alphabet;
} KWG;

KWG * create_kwg(const char* kwg_filename, const char* alphabet_filename);
void destroy_kwg(KWG * kwg);
int kwg_accepts(KWG * kwg, int node_index);
int kwg_arc_index(KWG * kwg, int node_index);
int kwg_tile(KWG * kwg, int node_index);
int kwg_get_root_node_index(KWG * kwg);
int kwg_is_end(KWG * kwg, int node_index);
int kwg_get_next_node_index(KWG * kwg, int node_index, int letter);
int kwg_in_letter_set(KWG * kwg, int letter, int node_index);
int kwg_get_letter_set(KWG * kwg, int node_index);

#endif