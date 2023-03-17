#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "alphabet.h"
#include "constants.h"
#include "leaves.h"
#include "rack.h"

int get_add_edge_index(int node_index, uint8_t letter, int number_of_unique_tiles) {
    return (node_index * (number_of_unique_tiles) * 2) + letter;
}

int get_take_edge_index(int node_index, uint8_t letter, int number_of_unique_tiles) {
    return (node_index * (number_of_unique_tiles) * 2) + (number_of_unique_tiles) + letter;
}

double get_current_value(Laddag * laddag) {
    return laddag->values[laddag->current_index];
}

void traverse_add_edge(Laddag * laddag, uint8_t letter) {
    laddag->current_index = laddag->edges[get_add_edge_index(laddag->current_index, letter, laddag->number_of_unique_tiles)];
}

void traverse_take_edge(Laddag * laddag, uint8_t letter) {
    laddag->current_index = laddag->edges[get_take_edge_index(laddag->current_index, letter, laddag->number_of_unique_tiles)];
}

void go_to_leave(Laddag * laddag, Rack * rack) {
    laddag->current_index = 0;
    for (int i = 0; i < (laddag->number_of_unique_tiles); i++) {
        for (int j = 0; j < rack->array[i]; j++) {
            traverse_add_edge(laddag, i);
        }
    }
}

void set_start_leave(Laddag * laddag, Rack * rack) {
    // If the rack is less than RACK_SIZE tiles, the
    // leave is already in the leave graph and
    // we just have to find the index. If the
    // rack is RACK_SIZE tiles, we have to create the
    // RACK_SIZE tiles leave nodes in the leave graph.
    if (rack->number_of_letters < (RACK_SIZE)) {
        go_to_leave(laddag, rack);
        return;
    }
    int start_leave_index = laddag->number_of_nodes - 1;
    int full_rack_minus_one_add_edge_index;
    laddag->values[start_leave_index] = 0;
    for (int i = 0; i < (laddag->number_of_unique_tiles); i++) {
        int start_leave_add_edge_index = get_add_edge_index(start_leave_index, i, laddag->number_of_unique_tiles);
        // Here we use the number of values as an invalid node.
        laddag->edges[start_leave_add_edge_index] = laddag->number_of_nodes;
        int start_leave_take_edge_index = get_take_edge_index(start_leave_index, i, laddag->number_of_unique_tiles);
        if (rack->array[i] == 0) {
            laddag->edges[start_leave_take_edge_index] = laddag->number_of_nodes;
        } else {
            take_letter_from_rack(rack, i);
            go_to_leave(laddag, rack);
            // The laddag->current_index is now the full rack minus this machine letter.
            // Set the take edge for the full rack and the add edge for the 
            // full rack minus this machine letter.
            full_rack_minus_one_add_edge_index = get_add_edge_index(laddag->current_index, i, laddag->number_of_unique_tiles);
            laddag->edges[start_leave_take_edge_index] = laddag->current_index;
            laddag->edges[full_rack_minus_one_add_edge_index] = start_leave_index;
            add_letter_to_rack(rack, i);
        }
    }
	laddag->current_index = start_leave_index;
}

typedef union ReadDouble {
   uint64_t i;
   double d;
} ReadDouble;

void load_laddag(Laddag * laddag, const char * laddag_filename, int number_of_unique_tiles) {
	FILE * stream;
	stream = fopen(laddag_filename, "r");
	if (stream == NULL) {
		perror(laddag_filename);
		exit(EXIT_FAILURE);
	}
	size_t result;

	char magic_string[5];
	result = fread(&magic_string, sizeof(char), 4, stream);
	if (result != 4) {
		printf("fread failure: %zd != %d", result, 4);
		exit(EXIT_FAILURE);
	}
	magic_string[4] = '\0';
	if (strcmp(magic_string, LADDAG_MAGIC_STRING) != 0) {
		printf("magic number does not match laddag: >%s< != >%s<", magic_string, LADDAG_MAGIC_STRING);
		exit(EXIT_FAILURE);
	}

	uint8_t lexicon_name_length;
	result = fread(&lexicon_name_length, sizeof(lexicon_name_length), 1, stream);
	if (result != 1) {
		printf("fread failure: %zd != %d", result, 1);
		exit(EXIT_FAILURE);
	}

	char lexicon_name[lexicon_name_length+1];
	lexicon_name[lexicon_name_length] = '\0';
	result = fread(&lexicon_name, sizeof(char), lexicon_name_length, stream);
	if (result != lexicon_name_length) {
		printf("fread failure: %zd != %d", result, lexicon_name_length);
		exit(EXIT_FAILURE);
	}

    uint32_t number_of_nodes;
	result = fread(&number_of_nodes, sizeof(number_of_nodes), 1, stream);
	if (result != 1) {
		printf("nodes fread failure: %zd != %d", result, 1);
		exit(EXIT_FAILURE);
	}
	number_of_nodes = be32toh(number_of_nodes);

    uint32_t number_of_edges = number_of_nodes * 2 * (number_of_unique_tiles);

	laddag->edges = (uint32_t *) malloc(number_of_edges*sizeof(uint32_t));
	result = fread(laddag->edges, sizeof(uint32_t), number_of_edges, stream);
	if (result != number_of_edges) {
		printf("edges fread failure: %zd != %d", result, number_of_edges);
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < number_of_edges; i++) {
		laddag->edges[i] = be32toh(laddag->edges[i]);
	}

	// First read the values as ints
	uint64_t * valuesAsInts =  (uint64_t *) malloc(number_of_nodes*sizeof(uint64_t));
	result = fread(valuesAsInts, sizeof(uint64_t), number_of_nodes, stream);
	if (result != number_of_nodes) {
		printf("vals fread failure: %zd != %d", result, number_of_nodes);
		exit(EXIT_FAILURE);
	}

	// Convert the ints to doubles using a union
	laddag->values = (double *) malloc(number_of_nodes*sizeof(double));
	ReadDouble rd;
	for (uint32_t i = 0; i < number_of_nodes; i++) {
		rd.i = be64toh(valuesAsInts[i]);
		laddag->values[i] = rd.d;
	}
	free(valuesAsInts);
	fclose(stream);
    laddag->current_index = 0;
    laddag->number_of_nodes = number_of_nodes;
	laddag->number_of_unique_tiles = number_of_unique_tiles;
}

Laddag * create_laddag(const char* laddag_filename, int number_of_unique_tiles) {
    Laddag * laddag =  malloc(sizeof(Laddag));
    load_laddag(laddag, laddag_filename, number_of_unique_tiles);
    return laddag;
}

void destroy_laddag(Laddag * laddag) {
    free(laddag->edges);
    free(laddag->values);
    free(laddag);
}