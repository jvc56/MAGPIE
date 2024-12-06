#ifndef MAGGI_COLORS_H
#define MAGGI_COLORS_H

#include <stdint.h>

#include "raylib/src/raylib.h"

#include "../src/def/letter_distribution_defs.h"

// For various backgrounds
#define GRAY8PERCENT                                                           \
  CLITERAL(Color) { 0.08 * 255, 0.08 * 255, 0.08 * 255, 255 }
#define GRAY16PERCENT                                                          \
  CLITERAL(Color) { 0.16 * 255, 0.16 * 255, 0.16 * 255, 255 }

// For empty nonpremium squares
#define GRAY20PERCENT                                                          \
  CLITERAL(Color) { 0.20 * 255, 0.20 * 255, 0.20 * 255, 255 }

#define GRAY40PERCENT                                                          \
  CLITERAL(Color) { 0.40 * 255, 0.40 * 255, 0.40 * 255, 255 }
  
// For tiles
#define GOLDEN                                                                 \
  CLITERAL(Color){ 240, 220, 180, 255 }
// For square around designated blank letters
#define DARKRED CLITERAL(Color){ 72, 16, 16, 255 } 

// Premium squares
#define PREMIUM_RED                                                            \
  CLITERAL(Color) { 100, 50, 50, 255 }
#define PREMIUM_DARKBLUE                                                       \
  CLITERAL(Color) { 40, 80, 140, 255 }
#define PREMIUM_PINK                                                           \
  CLITERAL(Color) { 140, 80, 80, 255 }
#define PREMIUM_LIGHTBLUE                                                      \
  CLITERAL(Color) { 120, 140, 180, 255 }

static inline Color get_square_background_color(uint8_t ml, uint8_t bonus_square) {
  if (ml != ALPHABET_EMPTY_SQUARE_MARKER) {
    return GOLDEN;
  }
  const uint8_t letter_mul = bonus_square & 0x0F;
  const uint8_t word_mul = bonus_square >> 4;
  if (word_mul == 3) {
    return PREMIUM_RED;
  }
  if (word_mul == 2) {
    return PREMIUM_PINK;
  }
  if (letter_mul == 3) {
    return PREMIUM_DARKBLUE;
  }
  if (letter_mul == 2) {
    return PREMIUM_LIGHTBLUE;
  }
  return GRAY20PERCENT;
}

#endif // MAGGI_COLORS_H