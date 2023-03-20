#ifndef ALPHABET_H
#define ALPHABET_H

#include <stdint.h>

#include "constants.h"

typedef struct Alphabet {
    int size;
    // These (vals and letters) are arrays that
    // behave like maps
    // letters maps machine letters to char values
    // vals maps char values to machine letters
    uint32_t * vals;
    uint32_t * letters;
} Alphabet;

Alphabet * create_alphabet_from_file(const char* alphabet_filename);
void destroy_alphabet(Alphabet * alphabet);
uint8_t get_blanked_machine_letter(uint8_t ml);
uint8_t get_unblanked_machine_letter(uint8_t letter);
uint8_t is_blanked(uint8_t ml);
int is_vowel(uint8_t ml, Alphabet * alphabet);
uint8_t val(Alphabet * alphabet, unsigned char r);
unsigned char user_visible_letter(Alphabet * alphabet, uint8_t ml);

#endif