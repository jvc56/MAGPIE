#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "letter_distribution.h"

void load_letter_distribution(LetterDistribution * letter_distribution, const char* letter_distribution_filename) {
	FILE * stream;
	stream = fopen(letter_distribution_filename, "r");
	if (stream == NULL) {
		perror(letter_distribution_filename);
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

	result = fread(&letter_distribution->size, sizeof(uint32_t), 1, stream);
	if (result != 1) {
		printf("fread failure: %zd != %d", result, 1);
		exit(EXIT_FAILURE);
	}
	letter_distribution->size = be32toh(letter_distribution->size);

	letter_distribution->distribution = (uint32_t *) malloc(letter_distribution->size*sizeof(uint32_t));
	result = fread(letter_distribution->distribution, sizeof(uint32_t), letter_distribution->size, stream);
	if (result != letter_distribution->size) {
		printf("fread failure: %zd != %d", result, letter_distribution->size);
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < letter_distribution->size; i++) {
		letter_distribution->distribution[i] = be32toh(letter_distribution->distribution[i]);
	}

	letter_distribution->scores = (uint32_t *) malloc(letter_distribution->size*sizeof(uint32_t));
	result = fread(letter_distribution->scores, sizeof(uint32_t), letter_distribution->size, stream);
	if (result != letter_distribution->size) {
		printf("fread failure: %zd != %d", result, letter_distribution->size);
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < letter_distribution->size; i++) {
		letter_distribution->scores[i] = be32toh(letter_distribution->scores[i]);
	}

	letter_distribution->is_vowel = (uint32_t *) malloc(letter_distribution->size*sizeof(uint32_t));
	result = fread(letter_distribution->is_vowel, sizeof(uint32_t), letter_distribution->size, stream);
	if (result != letter_distribution->size) {
		printf("fread failure: %zd != %d", result, letter_distribution->size);
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < letter_distribution->size; i++) {
		letter_distribution->is_vowel[i] = be32toh(letter_distribution->is_vowel[i]);
	}

	uint32_t * score_indexes = (uint32_t *) malloc(letter_distribution->size*2*sizeof(uint32_t));
	for (uint32_t i = 0; i < letter_distribution->size; i++) {
		score_indexes[i*2] = i;
		score_indexes[i*2+1] = letter_distribution->scores[i];
	}

	// There's probably a better way, but I didn't want to create
	// a new struct just for this and it's not on the critical path.
    uint32_t i = 1;
    int k;
    uint32_t index_x;
	uint32_t score_x;
    while (i < letter_distribution->size) {
        index_x = score_indexes[i*2];
        score_x = score_indexes[i*2+1];
        k = i - 1;
        while (k >= 0 && score_x > score_indexes[k*2+1]) {
        	score_indexes[(k+1)*2] = score_indexes[(k)*2];
        	score_indexes[(k+1)*2+1] = score_indexes[(k)*2+1];
            k--;
        }
        score_indexes[(k+1)*2] = index_x;
        score_indexes[(k+1)*2+1] = score_x;
        i++;
    }

	letter_distribution->score_order = (uint32_t *) malloc(letter_distribution->size*sizeof(uint32_t));
	for (uint32_t i = 0; i < letter_distribution->size; i++) {
		letter_distribution->score_order[i] = score_indexes[i*2];
	}

	free(score_indexes);

	fclose(stream);
}

LetterDistribution * create_letter_distribution(const char* filename) {
	LetterDistribution * letter_distribution = malloc(sizeof(LetterDistribution));
    load_letter_distribution(letter_distribution, filename);
    return letter_distribution;
}

void destroy_letter_distribution(LetterDistribution * letter_distribution) {
	free(letter_distribution->distribution);
	free(letter_distribution->scores);
	free(letter_distribution->is_vowel);
	free(letter_distribution);
}