#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "letter_distribution.h"

extern inline uint8_t get_blanked_machine_letter(uint8_t ml);
extern inline uint8_t get_unblanked_machine_letter(uint8_t ml);
extern inline uint8_t is_blanked(uint8_t ml);

int count_number_of_newline_characters_in_file(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        printf("Error opening file to count lines: %s\n", filename);
        return -1;
    }

    int line_count = 0;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\n') {
            line_count++;
        }
    }

    fclose(file);
    return line_count;
}

void load_letter_distribution(LetterDistribution * letter_distribution, const char* letter_distribution_filename) {
	// This function call opens and closes the file, so
	// call it before the fopen to prevent a nested file read
    letter_distribution->size = count_number_of_newline_characters_in_file(letter_distribution_filename) + 1;

    FILE* file = fopen(letter_distribution_filename, "r");
    if (file == NULL) {
        printf("Error opening letter distribution file: %s\n", letter_distribution_filename);
        abort();
    }

	letter_distribution->distribution = (uint32_t *) malloc(letter_distribution->size*sizeof(uint32_t));
	letter_distribution->scores = (uint32_t *) malloc(letter_distribution->size*sizeof(uint32_t));
	letter_distribution->score_order = (uint32_t *) malloc(letter_distribution->size*sizeof(uint32_t));
	letter_distribution->is_vowel = (uint32_t *) malloc(letter_distribution->size*sizeof(uint32_t));
	letter_distribution->human_readable_letter_to_machine_letter = (uint32_t *) malloc((MACHINE_LETTER_MAX_VALUE + 1)*sizeof(uint32_t));
	letter_distribution->machine_letter_to_human_readable_letter = (uint32_t *) malloc((MACHINE_LETTER_MAX_VALUE + 1)*sizeof(uint32_t));

	int machine_letter = 0;
    char line[100];
    while (fgets(line, sizeof(line), file)) {
		// For now, we assume 1 char == 1 letter
		// This does not hold true for all languages
		// and will have to be updated.

        char* token;

		// letter, lower case, dist, score, is_vowel
        token = strtok(line, ",");
        char letter = token[0];

        token = strtok(NULL, ",");
        char lower_case_letter = token[0];

        token = strtok(NULL, ",");
		int dist = atoi(token);

		token = strtok(NULL, ",");
		int score = atoi(token);

		token = strtok(NULL, ",");
		int is_vowel = atoi(token);

		letter_distribution->distribution[machine_letter] = dist;
		letter_distribution->scores[machine_letter] = score;
		letter_distribution->is_vowel[machine_letter] = is_vowel;

		letter_distribution->human_readable_letter_to_machine_letter[(int)letter] = machine_letter;
		letter_distribution->machine_letter_to_human_readable_letter[machine_letter] = letter;

		if (machine_letter > 0) {
			uint8_t blanked_machine_letter = get_blanked_machine_letter(machine_letter);
			letter_distribution->human_readable_letter_to_machine_letter[(int)lower_case_letter] = blanked_machine_letter;
			letter_distribution->machine_letter_to_human_readable_letter[blanked_machine_letter] = lower_case_letter;
		}

		int i = machine_letter;
		for (; i > 0 && (int)letter_distribution->scores[(int)letter_distribution->score_order[i-1]] < score; i--) {
			letter_distribution->score_order[i] = letter_distribution->score_order[i-1];
		}
		letter_distribution->score_order[i] = machine_letter;

		machine_letter++;
    }

    fclose(file);
}

// Assumes english
uint8_t human_readable_letter_to_machine_letter(LetterDistribution * letter_distribution, unsigned char r) {
    return (uint8_t)letter_distribution->human_readable_letter_to_machine_letter[r];
}

unsigned char machine_letter_to_human_readable_letter(LetterDistribution * letter_distribution, uint8_t ml) {
    return (unsigned char)letter_distribution->machine_letter_to_human_readable_letter[ml];
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
	free(letter_distribution->score_order);
	free(letter_distribution->human_readable_letter_to_machine_letter);
	free(letter_distribution->machine_letter_to_human_readable_letter);
	free(letter_distribution);
}