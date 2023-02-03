#ifndef GADDAG_H
#define GADDAG_H

#include <stdint.h>

#include "alphabet.h"

typedef struct Gaddag {
    uint32_t * nodes;
    uint64_t * letter_sets;
    Alphabet * alphabet;
} Gaddag;

Gaddag* create_gaddag(const char* gaddag_filename, const char* alphabet_filename);
void destroy_gaddag(Gaddag * gaddag);
uint64_t get_letter_set(Gaddag* gaddag, uint32_t node_index);
int in_letter_set(Gaddag* gaddag, uint8_t letter, uint32_t node_index);
uint32_t get_next_node_index(Gaddag * gaddag, uint32_t node_index, uint8_t letter);
uint8_t get_number_of_arcs(Gaddag * gaddag, uint32_t node_index);

#endif