#include <string.h>

#include "../src/letter_distribution.h"
#include "test_util.h"

#include "alphabet_print.h"

void write_user_visible_letter_to_end_of_buffer(
    char *dest, LetterDistribution *letter_distribution, uint8_t ml) {

  char human_letter[MAX_LETTER_CHAR_LENGTH];
  machine_letter_to_human_readable_letter(letter_distribution, ml,
                                          human_letter);
  for (size_t i = 0; i < strlen(human_letter); i++) {
    write_char_to_end_of_buffer(dest, human_letter[i]);
  }
}