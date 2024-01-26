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

uint32_t kwg_node(const KWG *kwg, int node_index);
bool kwg_node_is_end(uint32_t node);
bool kwg_node_accepts(uint32_t node);
uint32_t kwg_node_arc_index(uint32_t node);
int kwg_node_tile(uint32_t node);
int kwg_get_dawg_root_node_index(const KWG *kwg);
int kwg_get_root_node_index(const KWG *kwg);
int kwg_get_next_node_index(const KWG *kwg, int node_index, int letter);
bool kwg_in_letter_set(const KWG *kwg, int letter, int node_index);
uint64_t kwg_get_letter_set(const KWG *kwg, int node_index);

#endif