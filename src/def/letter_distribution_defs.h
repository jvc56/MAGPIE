#ifndef LETTER_DISTRIBUTION_DEFS_H
#define LETTER_DISTRIBUTION_DEFS_H

#include <stdint.h>

typedef uint8_t MachineLetter;

#define ALPHABET_EMPTY_SQUARE_MARKER 0
#define PLAYED_THROUGH_MARKER 0
#define BLANK_MACHINE_LETTER 0
#define ASCII_PLAYED_THROUGH '.'
#define ASCII_UCGI_PLAYED_THROUGH '$'

#define BLANK_MASK 0x80
#define UNBLANK_MASK (0x80 - 1)
#define MAX_ALPHABET_SIZE 50
#define MACHINE_LETTER_MAX_VALUE (MAX_ALPHABET_SIZE + BLANK_MASK)
#define MAX_LETTER_BYTE_LENGTH 6

#define ENGLISH_LETTER_DISTRIBUTION_NAME "english"
#define GERMAN_LETTER_DISTRIBUTION_NAME "german"
#define NORWEGIAN_LETTER_DISTRIBUTION_NAME "norwegian"
#define CATALAN_LETTER_DISTRIBUTION_NAME "catalan"
#define POLISH_LETTER_DISTRIBUTION_NAME "polish"
#define DUTCH_LETTER_DISTRIBUTION_NAME "dutch"
#define FRENCH_LETTER_DISTRIBUTION_NAME "french"

#define SUPER_LETTER_DISTRIBUTION_NAME_EXTENSION "super"

#endif
