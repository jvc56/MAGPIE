#ifndef BONUS_SQUARE_H
#define BONUS_SQUARE_H

#include <stdbool.h>
#include <stdint.h>

#define BRICK_VALUE 0xFF
#define BRICK_CHAR '#'
#define BONUS_SQUARE_MAP_SIZE 256
#define BONUS_SQUARE_CHAR_NONE ' '
#define BONUS_SQUARE_CHAR_DOUBLE_LETTER '\''
#define BONUS_SQUARE_CHAR_DOUBLE_WORD '-'
#define BONUS_SQUARE_CHAR_TRIPLE_LETTER '"'
#define BONUS_SQUARE_CHAR_TRIPLE_WORD '='
#define BONUS_SQUARE_CHAR_QUADRUPLE_LETTER '^'
#define BONUS_SQUARE_CHAR_QUADRUPLE_WORD '~'

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

static inline BonusSquare bonus_square_from_char(char bonus_square_char) {
  return (BonusSquare){
      // cppcheck-suppress unreadVariable
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
