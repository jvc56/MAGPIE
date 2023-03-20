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

    size_t number_of_nodes = kwg_size / sizeof(uint32_t);

	size_t result;

    kwg->nodes = (uint32_t *) malloc(number_of_nodes*sizeof(uint32_t));
	result = fread(kwg->nodes, sizeof(uint32_t), number_of_nodes, stream);
	if (result != number_of_nodes) {
		printf("fread failure: %zd != %zd", result, number_of_nodes);
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < number_of_nodes; i++) {
		kwg->nodes[i] = le32toh(kwg->nodes[i]);
	}
	fclose(stream);

    // Alphabet size is not needed for kwg.
	kwg->alphabet = create_alphabet_from_file(alphabet_filename);
}

KWG * create_kwg(const char* kwg_filename, const char* alphabet_filename) {
    KWG * kwg =  malloc(sizeof(KWG));
    load_kwg(kwg, kwg_filename, alphabet_filename);
    return kwg;
}

void destroy_kwg(KWG * kwg) {
    destroy_alphabet(kwg->alphabet);
    free(kwg->nodes);
    free(kwg);
}

int kwg_is_end(KWG * kwg, int node_index) {
	return (kwg->nodes[node_index] & 0x400000) != 0;
}

int kwg_accepts(KWG * kwg, int node_index) {
	return (kwg->nodes[node_index] & 0x800000) != 0;
}

int kwg_arc_index(KWG * kwg, int node_index) {
	return (kwg->nodes[node_index] & 0x3fffff);
}

int kwg_tile(KWG * kwg, int node_index) {
	return kwg->nodes[node_index] >> 24;
}

int kwg_get_root_node_index(KWG * kwg) {
    return kwg_arc_index(kwg, 1);
}

int kwg_get_next_node_index(KWG * kwg, int node_index, int letter) {
    int i = node_index;
    while (1)
    {
        if (kwg_tile(kwg, i) == letter) {
            return kwg_arc_index(kwg, i);
        }
        if (kwg_is_end(kwg, i)) {
            return 0;
        }
        i++;
    }
}

int kwg_in_letter_set(KWG * kwg, int letter, int node_index) {
    letter = get_unblanked_machine_letter(letter);
    int i = node_index;
    while (1)
    {
        if (kwg_tile(kwg, i) == letter) {
            return kwg_accepts(kwg, i);
        }
        if (kwg_is_end(kwg, i)) {
            return 0;
        }
        i++;
    }
}

int kwg_get_letter_set(KWG * kwg, int node_index) {
    int ls = 0;
    int i = node_index;
    while (1)
    {
        int t = kwg_tile(kwg, i);
        if (kwg_accepts(kwg, i)) {
            ls |= (1 << t);
        }
        if (kwg_is_end(kwg, i)) {
            break;
        }
        i++;
    }
    return ls;
}

