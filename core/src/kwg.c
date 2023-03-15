#include <stdlib.h>
#include <stdio.h>

#include "alphabet.h"
#include "kwg.h"

void load_kwg(KWG * kwg, const char* kwg_filename, const char* alphabet_filename) {
	FILE * stream;
	stream = fopen(kwg_filename, "r");
	if (stream == NULL) {
		perror(kwg_filename);
		exit(EXIT_FAILURE);
	}

    fseek(stream, 0, SEEK_END); // seek to end of file
    long int kwg_size = ftell(stream); // get current file pointer
    fseek(stream, 0, SEEK_SET);

    int number_of_nodes = kwg_size / sizeof(uint32_t);

	size_t result;

    kwg->nodes = (uint32_t *) malloc(number_of_nodes*sizeof(uint32_t));
	result = fread(kwg->nodes, sizeof(uint32_t), number_of_nodes, stream);
	if (result != number_of_nodes) {
		printf("fread failure: %zd != %d", result, number_of_nodes);
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < number_of_nodes; i++) {
		kwg->nodes[i] = le32toh(kwg->nodes[i]);
	}

    // Alphabet size is not needed for kwg.
	kwg->alphabet = create_alphabet_from_file(alphabet_filename, 0);

    return kwg;
}

KWG * create_kwg(const char* kwg_filename, const char* alphabet_filename) {
    KWG * kwg =  malloc(sizeof(KWG));
    load_kwg(kwg, kwg_filename);
    return kwg;
}

void destroy_kwg(KWG * kwg) {
    free(kwg->nodes);
    free(kwg);
}

int is_end(KWG * kwg, int node_index) {
	return kwg->nodes[node_index] & 0x400000 != 0;
}

int accepts(KWG * kwg, int node_index) {
	return kwg->nodes[node_index] & 0x800000 != 0;
}

int arc_index(KWG * kwg, int node_index) {
	return kwg->nodes[node_index] & 0x3fffff;
}

int tile(KWG * kwg, int node_index) {
	return kwg->nodes[node_index] >> 24;
}

int kwg_get_root_node_index(KWG * kwg) {
    return arc_index(kwg, 1);
}

int kwg_next_node_index(KWG * kwg, int node_index, int letter) {
    int i = node_index;
    while (1)
    {
        if (tile(kwg, i) == letter) {
            return arc_index(kwg, i);
        }
        if (is_end(kwg, i)) {
            return 0;
        }
        i++;
    }
}

int kwg_in_letter_set(KWG * kwg, int letter, int node_index) {
    letter = get_unblanked_machine_letter(letter);
    while (1)
    {
        if (tile(kwg, i) == letter) {
            return accepts(kwg, i);
        }
        if (is_end(kwg, i)) {
            return 0;
        }
        i++;
    }
}

int kwg_get_letter_set(KWG * kwg, int node_index) {
    int ls = 0;
    while (1)
    {
        int t = tile(kwg, i);
        if (accepts(kwg, i)) {
            ls |= (1 << t);
        }
        if (is_end(kwg, i)) {
            break;
        }
        i++;
    }
    return ls;
}

