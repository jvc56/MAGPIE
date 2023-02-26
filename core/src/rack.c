#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "rack.h"

void reset_rack(Rack * rack) {
	for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
		rack->array[i] = 0;
	}
	rack->empty = 1;
	rack->number_of_letters = 0;
	rack->number_of_nonzero_indexes = 0;
}

Rack * create_rack() {
    Rack * rack = malloc(sizeof(Rack));
	for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
		rack->letter_to_array_nonzero_index[i] = 0;
	}
	for (int i = 0; i < (RACK_SIZE); i++) {
		rack->array_nonzero_indexes[i] = 0;
	}
	reset_rack(rack);
	return rack;
}

void destroy_rack(Rack * rack) {
    free(rack);
}

int take_letter_from_rack(Rack * rack, uint8_t letter) {
	rack->array[letter]--;
	rack->number_of_letters--;
	if (rack->number_of_letters == 0) {
		rack->empty = 1;
	}
	int this_letter_array_nonzero_index = -1;
	if (rack->array[letter] == 0) {
		this_letter_array_nonzero_index = rack->letter_to_array_nonzero_index[letter];
		int replacement_letter = rack->array_nonzero_indexes[rack->number_of_nonzero_indexes - 1];
		rack->array_nonzero_indexes[this_letter_array_nonzero_index] = replacement_letter;
		rack->letter_to_array_nonzero_index[replacement_letter] = this_letter_array_nonzero_index;
		rack->number_of_nonzero_indexes--;
	}
	return this_letter_array_nonzero_index;
}

void add_letter_to_rack(Rack * rack, uint8_t letter, int nonzero_array_index) {
	rack->array[letter]++;
	rack->number_of_letters++;
	if (rack->empty == 1) {
		rack->empty = 0;
	}
	if (rack->array[letter] == 1) {
		// In this case, we are replacing an existing letter which
		// should be moved to the end.
		if (nonzero_array_index < rack->number_of_nonzero_indexes) {
			int previous_letter = rack->array_nonzero_indexes[nonzero_array_index];
			rack->array_nonzero_indexes[rack->number_of_nonzero_indexes] = previous_letter;
			rack->letter_to_array_nonzero_index[previous_letter] = rack->number_of_nonzero_indexes;
		}
		// Write the new letter.
		rack->array_nonzero_indexes[nonzero_array_index] = letter;
		rack->letter_to_array_nonzero_index[letter] = nonzero_array_index;
		rack->number_of_nonzero_indexes++;
	}
}

int score_on_rack(LetterDistribution * letter_distribution, Rack * rack) {
    int sum = 0;
    for (int i = 0; i < (RACK_ARRAY_SIZE); i++) {
        sum += rack->array[i] * letter_distribution->scores[i];
    }
    return sum;
}

void set_rack_to_string(Rack * rack, const char* rack_string, Alphabet * alphabet) {
    reset_rack(rack);
    for (size_t i = 0; i < strlen(rack_string); i++) {
        add_letter_to_rack(rack, val(alphabet, rack_string[i]), rack->number_of_nonzero_indexes);
    }
}
