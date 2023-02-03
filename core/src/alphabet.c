#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alphabet.h"
#include "constants.h"

void load_alphabet(Alphabet * alphabet, const char * alphabet_filename) {
	FILE * stream;
	stream = fopen(alphabet_filename, "r");
	if (stream == NULL) {
		perror(alphabet_filename);
		abort();
	}
	size_t result;

	char magic_string[5];
	result = fread(&magic_string, sizeof(char), 4, stream);
	if (result != 4) {
		printf("fread failure: %zd != %d", result, 4);
		exit(EXIT_FAILURE);
	}
	magic_string[4] = '\0';
	if (strcmp(magic_string, ALPHABET_MAGIC_STRING) != 0) {
		printf("magic number does not match laddag: >%s< != >%s<", magic_string, ALPHABET_MAGIC_STRING);
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

	uint32_t letters_size;
	result = fread(&letters_size, sizeof(letters_size), 1, stream);
	if (result != 1) {
		printf("fread failure: %zd != %d", result, 1);
		exit(EXIT_FAILURE);
	}
	letters_size = be32toh(letters_size);

	alphabet->letters = (uint32_t *) malloc(letters_size*sizeof(uint32_t));
	result = fread(alphabet->letters, sizeof(uint32_t), letters_size, stream);
	if (result != letters_size) {
		printf("fread failure: %zd != %d", result, letters_size);
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < letters_size; i++) {
		alphabet->letters[i] = be32toh(alphabet->letters[i]);
	}

	uint32_t vals_size;
	result = fread(&vals_size, sizeof(vals_size), 1, stream);
	if (result != 1) {
		printf("fread failure: %zd != %d", result, 1);
		exit(EXIT_FAILURE);
	}
	vals_size = be32toh(vals_size);


	alphabet->vals = (uint32_t *) malloc(vals_size*sizeof(uint32_t));
	result = fread(alphabet->vals, sizeof(uint32_t), vals_size, stream);
	if (result != vals_size) {
		printf("fread failure: %zd != %d", result, vals_size);
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < vals_size; i++) {
		alphabet->vals[i] = be32toh(alphabet->vals[i]);
	}
	fclose(stream);
}

void from_slice(Alphabet * alphabet, uint32_t array[], uint32_t letters_size) {
	alphabet->size = letters_size;
	for (int i = 0; i < alphabet->size; i++) {
		alphabet->vals[array[i]] = i;
		alphabet->letters[i] = array[i];
	}
}

Alphabet * create_alphabet_from_slice(uint32_t array[], uint32_t letters_size) {
	Alphabet * alphabet = malloc(sizeof(Alphabet));
	from_slice(alphabet, array, letters_size);
	return alphabet;
}

Alphabet * create_alphabet_from_file(const char* alphabet_filename, int alphabet_size) {
	Alphabet * alphabet = malloc(sizeof(Alphabet));
	load_alphabet(alphabet, alphabet_filename);
	alphabet->size = alphabet_size;
	return alphabet;
}

void destroy_alphabet(Alphabet * alphabet) {
	free(alphabet->letters);
	free(alphabet->vals);
	free(alphabet);
}

int get_number_of_letters(Alphabet * alphabet) {
	return alphabet->size;
}

uint8_t get_blanked_machine_letter(uint8_t ml) {
	if (ml < BLANK_OFFSET) {
		return ml + BLANK_OFFSET;
	}
	return ml;
}

uint8_t get_unblanked_machine_letter(uint8_t ml) {
	if (ml >= BLANK_OFFSET) {
		return ml - BLANK_OFFSET;
	}
	return ml;
}

int is_vowel(uint8_t ml, Alphabet * alphabet) {
	ml = get_unblanked_machine_letter(ml);
	uint8_t rn = alphabet->letters[ml];
	return rn == 'A' || rn == 'E' || rn == 'I' || rn == 'O' || rn == 'U';
}

// Assumes english
uint8_t val(Alphabet * alphabet, unsigned char r) {
    return (uint8_t)alphabet->vals[r];
}

unsigned char user_visible_letter(Alphabet * alphabet, uint8_t ml) {
    return (unsigned char)alphabet->letters[ml];
}
