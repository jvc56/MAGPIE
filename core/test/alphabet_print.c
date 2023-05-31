#include "../src/letter_distribution.h"
#include "test_util.h"

#include "alphabet_print.h"

// Assumes english for now
void write_user_visible_letter_to_end_of_buffer(char * dest, LetterDistribution * letter_distribution, uint8_t ml) {
	write_char_to_end_of_buffer(dest, machine_letter_to_human_readable_letter(letter_distribution, ml));
}