#ifndef LEAVES_H
#define LEAVES_H

#include "alphabet.h"
#include "constants.h"
#include "rack.h"

typedef struct Laddag {
    int current_index;
    int number_of_nodes;
    int number_of_unique_tiles;
    uint32_t * edges;
    double * values;
} Laddag;

Laddag * create_laddag(const char* laddag_filename, int number_of_unique_tiles);
void destroy_laddag(Laddag * laddag);
double get_current_value(Laddag * laddag);
int get_add_edge_index(int node_index, uint8_t letter, int number_of_unique_tiles);
int get_take_edge_index(int node_index, uint8_t letter, int number_of_unique_tiles);
void go_to_leave(Laddag * laddag, Rack * rack);
void set_start_leave(Laddag * laddag, Rack * rack);
void traverse_add_edge(Laddag * laddag, uint8_t letter);
void traverse_take_edge(Laddag * laddag, uint8_t letter);

#endif