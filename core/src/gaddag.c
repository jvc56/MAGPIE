#include <endian.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "alphabet.h"
#include "constants.h"
#include "gaddag.h"

void load_gaddag(Gaddag* gaddag, const char* gaddag_filename, const char* alphabet_filename) {
	FILE * stream;
	stream = fopen(gaddag_filename, "r");
	if (stream == NULL) {
		perror(gaddag_filename);
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
	if (strcmp(magic_string, GADDAG_MAGIC_STRING) != 0) {
		printf("magic number does not match gaddag: >%s< != >%s<", magic_string, GADDAG_MAGIC_STRING);
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

	uint32_t alphabet_size;
	result = fread(&alphabet_size, sizeof(alphabet_size), 1, stream);
	if (result != 1) {
		printf("fread failure: %zd != %d", result, 1);
		exit(EXIT_FAILURE);
	}
	alphabet_size = be32toh(alphabet_size);

	uint32_t alphabet_array[MAX_ALPHABET_SIZE];
	result = fread(&alphabet_array, sizeof(uint32_t), alphabet_size, stream);
	if (result != alphabet_size) {
		printf("fread failure: %zd != %d", result, alphabet_size);
		exit(EXIT_FAILURE);
	}

	for (uint32_t i = 0; i < alphabet_size; i++) {
		alphabet_array[i] = be32toh(alphabet_array[i]);
	}

	uint32_t letter_set_size;
	result = fread(&letter_set_size, sizeof(letter_set_size), 1, stream);
	if (result != 1) {
		printf("fread failure: %zd != %d", result, 1);
		exit(EXIT_FAILURE);
	}
	letter_set_size = be32toh(letter_set_size);

	gaddag->letter_sets = (uint64_t *) malloc(letter_set_size*sizeof(uint64_t));
	result = fread(gaddag->letter_sets, sizeof(uint64_t), letter_set_size, stream);
	if (result != letter_set_size) {
		printf("fread failure: %zd != %d", result, letter_set_size);
		exit(EXIT_FAILURE);
	}
	for (uint64_t i = 0; i < letter_set_size; i++) {
		gaddag->letter_sets[i] = be64toh(gaddag->letter_sets[i]);
	}
    uint32_t nodes_size;
	result = fread(&nodes_size, sizeof(nodes_size), 1, stream);
	if (result != 1) {
		printf("fread failure: %zd != %d", result, 1);
		exit(EXIT_FAILURE);
	}
	nodes_size = be32toh(nodes_size);

	gaddag->nodes = (uint32_t *) malloc(nodes_size*sizeof(uint32_t));
	result = fread(gaddag->nodes, sizeof(uint32_t), nodes_size, stream);
	if (result != nodes_size) {
		printf("fread failure: %zd != %d", result, nodes_size);
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < nodes_size; i++) {
		gaddag->nodes[i] = be32toh(gaddag->nodes[i]);
	}
	
	// When loading the gaddag, typically we would load the alphabet
	// as well. Since I don't want to bother with the unicode
	// ToUpper and ToLower functions in C, the full map of all possible
	// machine letter <-> char conversions is precomputed in Go and then
	// loaded as a separate file.

	// This line loads the alphabet from the gaddag
	// If uncommented, other things will break since letters and vals
	// no longer contain the full mapping of all values.
	// gaddag->alphabet = create_alphabet_from_slice(alphabet_array, alphabet_size);

	gaddag->alphabet = create_alphabet_from_file(alphabet_filename, alphabet_size);

	fclose(stream);
}

int gaddag_get_number_of_arcs(Gaddag* gaddag, uint32_t node_index) {
	return gaddag->nodes[node_index] >> GADDAG_NUM_ARCS_BIT_LOC;
}

uint32_t gaddag_get_next_node_index(Gaddag* gaddag, uint32_t node_index, uint8_t letter) {
	int number_of_arcs = get_number_of_arcs(gaddag, node_index);
	for (uint32_t k = node_index + 1; k <= number_of_arcs + node_index; k++) {
		int ml = (gaddag->nodes[k] >> GADDAG_LETTER_BIT_LOC);
		if (letter == ml) {
			return gaddag->nodes[k] & (GADDAG_NODE_IDX_BIT_MASK);
		}
	}
	return 0;
}

uint64_t gaddag_get_letter_set(Gaddag* gaddag, uint32_t node_index) {
	uint32_t letter_set_code = gaddag->nodes[node_index] & (LETTER_SET_BIT_MASK);
	return gaddag->letter_sets[letter_set_code];
}

int gaddag_in_letter_set(Gaddag* gaddag, uint8_t letter, uint32_t node_index) {
	// In the corresponding Macondo code, this function checks if
	// the letter is the separation machine letter. Here, we assume
	// that the letter is never the separation machine letter.
	// There does not seem to be a scenario where this happens.
	// if (letter == SEPARATION_MACHINE_LETTER) {
	// 	return 0;
	// }
	uint8_t ltc = letter;
	if (letter >= BLANK_OFFSET) {
		ltc = letter - BLANK_OFFSET;
	}
	uint64_t letter_set = get_letter_set(gaddag, node_index);
	return (letter_set & (1 << ltc)) != 0;
}

Gaddag* create_gaddag(const char* gaddag_filename, const char* alphabet_filename) {
	Gaddag * gaddag = malloc(sizeof(Gaddag));
	load_gaddag(gaddag, gaddag_filename, alphabet_filename);
	return gaddag;
}

void destroy_gaddag(Gaddag * gaddag) {
	destroy_alphabet(gaddag->alphabet);
	free(gaddag->letter_sets);
	free(gaddag->nodes);
	free(gaddag);
}
