#ifndef BONUS_SQUARE_H
#define BONUS_SQUARE_H

#include <stdbool.h>
#include <stdint.h>

enum {
  BRICK_VALUE = 0xFF,
  BRICK_CHAR = '#',
  BONUS_SQUARE_MAP_SIZE = 256,
  BONUS_SQUARE_CHAR_NONE = ' ',
  BONUS_SQUARE_CHAR_DOUBLE_LETTER = '\'',
  BONUS_SQUARE_CHAR_DOUBLE_WORD = '-',
  BONUS_SQUARE_CHAR_TRIPLE_LETTER = '"',
  BONUS_SQUARE_CHAR_TRIPLE_WORD = '=',
  BONUS_SQUARE_CHAR_QUADRUPLE_LETTER = '^',
  BONUS_SQUARE_CHAR_QUADRUPLE_WORD = '~'
};

static const uint8_t bonus_square_chars_to_raw_map[BONUS_SQUARE_MAP_SIZE] = {
    [BONUS_SQUARE_CHAR_NONE] = 0x11,
    [BONUS_SQUARE_CHAR_DOUBLE_LETTER] = 0x12,
    [BONUS_SQUARE_CHAR_DOUBLE_WORD] = 0x21,
    [BONUS_SQUARE_CHAR_TRIPLE_LETTER] = 0x13,
    [BONUS_SQUARE_CHAR_TRIPLE_WORD] = 0x31,
    [BONUS_SQUARE_CHAR_QUADRUPLE_LETTER] = 0x14,
    [BONUS_SQUARE_CHAR_QUADRUPLE_WORD] = 0x41,
    [BRICK_CHAR] = BRICK_VALUE,
};

static const char bonus_square_raw_to_chars_map[BONUS_SQUARE_MAP_SIZE] = {
    [0x11] = BONUS_SQUARE_CHAR_NONE,
    [0x12] = BONUS_SQUARE_CHAR_DOUBLE_LETTER,
    [0x21] = BONUS_SQUARE_CHAR_DOUBLE_WORD,
    [0x13] = BONUS_SQUARE_CHAR_TRIPLE_LETTER,
    [0x31] = BONUS_SQUARE_CHAR_TRIPLE_WORD,
    [0x14] = BONUS_SQUARE_CHAR_QUADRUPLE_LETTER,
    [0x41] = BONUS_SQUARE_CHAR_QUADRUPLE_WORD,
    [BRICK_VALUE] = BRICK_CHAR,
};

static const char* bonus_square_raw_to_color_codes[BONUS_SQUARE_MAP_SIZE] = {
    [0x11] = "\x1b[0m",     // none, reset
    [0x12] = "\x1b[1;36m",  // DLS, cyan
    [0x21] = "\x1b[1;35m",  // DWS, magenta
    [0x13] = "\x1b[1;34m",  // TLS, blue
    [0x31] = "\x1b[1;31m",  // TWS, red
    [0x14] = "\x1b[1;95m",  // QLS, bright magenta
    [0x41] = "\x1b[1;33m",  // QWS, yellow
    [BRICK_VALUE] = "\x1b[1;90m",  // brick, gray
};

static const char* bonus_square_raw_to_alt_strings[BONUS_SQUARE_MAP_SIZE] = {
    [0x11] = "　",  // ideographic space
    [0x12] = "＇",  // fullwidth apostrophe
    [0x21] = "－",  // fullwidth hyphen-minus
    [0x13] = "＂",  // fullwidth quotation mark
    [0x31] = "＝",  // fullwidth equals sign
    [0x14] = "＾",  // fullwidth circumflex accent
    [0x41] = "～",  // fullwidth tilde
    [BRICK_VALUE] = "＃",  // fullwidth number sign
};

typedef union {
  struct __attribute__((packed)) {
    uint8_t letter_multiplier : 4;
    uint8_t word_multiplier : 4;
  } multipliers;
  uint8_t raw;
} BonusSquare;

static inline char bonus_square_to_char(BonusSquare bonus_square) {
  return bonus_square_raw_to_chars_map[bonus_square.raw];
}

static inline const char *bonus_square_to_color_code(BonusSquare bonus_square) {
  return bonus_square_raw_to_color_codes[bonus_square.raw];
}

static inline const char *bonus_square_to_alt_string(BonusSquare bonus_square) {
  return bonus_square_raw_to_alt_strings[bonus_square.raw];
}

static inline BonusSquare bonus_square_from_char(char bonus_square_char) {
  return (BonusSquare){
      .raw = bonus_square_chars_to_raw_map[(int)bonus_square_char]};
}

static inline uint8_t
bonus_square_get_word_multiplier(BonusSquare bonus_square) {
  return bonus_square.multipliers.word_multiplier;
}

static inline uint8_t
bonus_square_get_letter_multiplier(BonusSquare bonus_square) {
  return bonus_square.multipliers.letter_multiplier;
}

static inline bool bonus_square_is_brick(BonusSquare bonus_square) {
  return bonus_square.raw == BRICK_VALUE;
}

static inline bool bonus_square_is_invalid(BonusSquare bonus_square) {
  return bonus_square.raw == 0;
}

static inline bool bonus_squares_are_equal(BonusSquare bs1, BonusSquare bs2) {
  return bs1.raw == bs2.raw;
}

#endif
