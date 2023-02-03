#include "../src/alphabet.h"
#include "test_util.h"

#include "alphabet_print.h"

// Assumes english for now
void write_user_visible_letter_to_end_of_buffer(char * dest, Alphabet * alphabet, uint8_t ml) {
	write_char_to_end_of_buffer(dest, user_visible_letter(alphabet, ml));
}