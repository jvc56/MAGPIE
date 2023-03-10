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

Alphabet * create_alphabet_from_file(const char* alphabet_filename, int alphabet_size);
Alphabet* create_alphabet_from_slice(uint32_t array[], uint32_t alphabet_size);
void destroy_alphabet(Alphabet * alphabet);
uint8_t get_blanked_machine_letter(uint8_t ml);
int get_number_of_letters(Alphabet * alphabet);
uint8_t get_unblanked_machine_letter(uint8_t letter);
int is_vowel(uint8_t ml, Alphabet * alphabet);
uint8_t val(Alphabet * alphabet, unsigned char r);
unsigned char user_visible_letter(Alphabet * alphabet, uint8_t ml);

#endif