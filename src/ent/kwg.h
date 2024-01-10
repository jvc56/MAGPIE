#ifndef KWG_H
#define KWG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct KWG KWG;

KWG *kwg_create(const char *kwg_name);
KWG *kwg_create_empty();
void kwg_destroy(KWG *kwg);
void kwg_read_nodes_from_stream(KWG *kwg, size_t number_of_nodes, FILE *stream);

bool kwg_is_end(const KWG *kwg, int node_index);
bool kwg_accepts(const KWG *kwg, int node_index);
int kwg_arc_index(const KWG *kwg, int node_index);
int kwg_tile(const KWG *kwg, int node_index);
int kwg_get_dawg_root_node_index(const KWG *kwg);
int kwg_get_root_node_index(const KWG *kwg);
int kwg_get_next_node_index(const KWG *kwg, int node_index, int letter);
bool kwg_in_letter_set(const KWG *kwg, int letter, int node_index);
uint64_t kwg_get_letter_set(const KWG *kwg, int node_index);

#endif