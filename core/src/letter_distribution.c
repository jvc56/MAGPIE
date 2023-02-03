#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "letter_distribution.h"

void load_letter_distribution(LetterDistribution * letter_distribution, const char* letter_distribution_filename) {
    for (int i = 0; i < (MACHINE_LETTER_MAX_VALUE + 1); i++) {
        letter_distribution->distribution[i] = 0;
        letter_distribution->scores[i] = 0;
        letter_distribution->is_vowel[i] = 0;
    }

	FILE * stream;
	stream = fopen(letter_distribution_filename, "r");
	if (stream == NULL) {
		perror(letter_distribution_filename);
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
	if (strcmp(magic_string, LETTER_DISTRIBUTION_MAGIC_STRING) != 0) {
		printf("magic number does not match letter_distribution: >%s< != >%s<", magic_string, LETTER_DISTRIBUTION_MAGIC_STRING);
		exit(EXIT_FAILURE);
	}

	uint8_t letter_distribution_name_length;
	result = fread(&letter_distribution_name_length, sizeof(letter_distribution_name_length), 1, stream);
	if (result != 1) {
		printf("fread failure: %zd != %d", result, 1);
		exit(EXIT_FAILURE);
	}

	char letter_distribution_name[letter_distribution_name_length+1];
	letter_distribution_name[letter_distribution_name_length] = '\0';
	result = fread(&letter_distribution_name, sizeof(char), letter_distribution_name_length, stream);
	if (result != letter_distribution_name_length) {
		printf("fread failure: %zd != %d", result, letter_distribution_name_length);
		exit(EXIT_FAILURE);
	}

	result = fread(letter_distribution->distribution, sizeof(uint32_t), (RACK_ARRAY_SIZE), stream);
	if (result != (RACK_ARRAY_SIZE)) {
		printf("fread failure: %zd != %d", result, (RACK_ARRAY_SIZE));
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < (RACK_ARRAY_SIZE); i++) {
		letter_distribution->distribution[i] = be32toh(letter_distribution->distribution[i]);
	}

	result = fread(letter_distribution->scores, sizeof(uint32_t), (RACK_ARRAY_SIZE), stream);
	if (result != (RACK_ARRAY_SIZE)) {
		printf("fread failure: %zd != %d", result, (RACK_ARRAY_SIZE));
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < (RACK_ARRAY_SIZE); i++) {
		letter_distribution->scores[i] = be32toh(letter_distribution->scores[i]);
	}

	result = fread(letter_distribution->is_vowel, sizeof(uint32_t), (RACK_ARRAY_SIZE), stream);
	if (result != (RACK_ARRAY_SIZE)) {
		printf("fread failure: %zd != %d", result, (RACK_ARRAY_SIZE));
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < (RACK_ARRAY_SIZE); i++) {
		letter_distribution->is_vowel[i] = be32toh(letter_distribution->is_vowel[i]);
	}
	fclose(stream);
}

LetterDistribution * create_letter_distribution(const char* filename) {
	LetterDistribution * letter_distribution = malloc(sizeof(LetterDistribution));
    load_letter_distribution(letter_distribution, filename);
    return letter_distribution;
}

void destroy_letter_distribution(LetterDistribution * letter_distribution) {
	free(letter_distribution);
}