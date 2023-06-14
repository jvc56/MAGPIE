#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "rack.h"
#include "letter_distribution.h"

void reset_rack(Rack * rack) {
	for (int i = 0; i < (rack->array_size); i++) {
		rack->array[i] = 0;
	}
	rack->empty = 1;
	rack->number_of_letters = 0;
}

Rack * create_rack(int array_size) {
    Rack * rack = malloc(sizeof(Rack));
	rack->array_size = array_size;
	rack->array = (int *) malloc(rack->array_size*sizeof(int));
	reset_rack(rack);
	return rack;
}

Rack * copy_rack(Rack * rack) {
    Rack * new_rack = malloc(sizeof(new_rack));
	new_rack->array_size = rack->array_size;
	new_rack->array = (int *) malloc(rack->array_size*sizeof(int));
	reset_rack(new_rack);

	for (int i = 0; i < rack->array_size; i++) {
		new_rack->array[i] = rack->array[i];
	}
	new_rack->number_of_letters = rack->number_of_letters;
	new_rack->empty = rack->empty;
	return new_rack;
}

void destroy_rack(Rack * rack) {
	free(rack->array);
    free(rack);
}

void take_letter_from_rack(Rack * rack, uint8_t letter) {
	rack->array[letter]--;
	rack->number_of_letters--;
	if (rack->number_of_letters == 0) {
		rack->empty = 1;
	}
}

void add_letter_to_rack(Rack * rack, uint8_t letter) {
	rack->array[letter]++;
	rack->number_of_letters++;
	if (rack->empty == 1) {
		rack->empty = 0;
	}
}

int score_on_rack(LetterDistribution * letter_distribution, Rack * rack) {
    int sum = 0;
    for (int i = 0; i < (rack->array_size); i++) {
        sum += rack->array[i] * letter_distribution->scores[i];
    }
    return sum;
}

void set_rack_to_string(Rack * rack, const char* rack_string, LetterDistribution * letter_distribution) {
    reset_rack(rack);
    for (size_t i = 0; i < strlen(rack_string); i++) {
        add_letter_to_rack(rack, human_readable_letter_to_machine_letter(letter_distribution, rack_string[i]));
    }
}
