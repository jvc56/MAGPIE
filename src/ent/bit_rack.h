#ifndef BIT_RACK_H
#define BIT_RACK_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "../def/bit_rack_defs.h"
#include "../def/board_defs.h"
#include "letter_distribution.h"
#include "rack.h"

// These are 128 bit integers representing multisets of tiles, used as keys for
// maps storing words formed by those tiles. They can include both undesignated
// blanks (from players' racks) and designated blanks (from the board).

// Arithmetic addition of two BitRacks performs unions of the multisets.

#if defined(__SIZEOF_INT128__)
#define USE_INT128_INTRINSIC 1
#else
#define USE_INT128_INTRINSIC 0
#endif

#if USE_INT128_INTRINSIC
typedef unsigned __int128 BitRack;
#else
typedef struct {
  uint64_t high;
  uint64_t low;
} BitRack;
#endif

bool bit_rack_is_compatible_with_ld(const LetterDistribution *ld) {
  int max_letter_count = 0;
  for (int ml = 0; ml < ld->size; ml++) {
    int letter_count = ld->distribution[ml];
    if (ml != BLANK_MACHINE_LETTER) {
      letter_count += ld->distribution[BLANK_MACHINE_LETTER];
    }
    if (letter_count > max_letter_count) {
      max_letter_count = letter_count;
    }
  }
  const int max_letter_count_on_board =
      max_letter_count < BOARD_DIM ? max_letter_count : BOARD_DIM;
  const int bit_rack_max_letter_count = (1 << BIT_RACK_BITS_PER_LETTER) - 1;

  return ld->size <= BIT_RACK_MAX_ALPHABET_SIZE &&
         max_letter_count_on_board <= bit_rack_max_letter_count;
}

BitRack bit_rack_create_empty(void) {
#if USE_INT128_INTRINSIC
  return 0;
#else
  return (BitRack){0, 0};
#endif
}

BitRack bit_rack_create_from_rack(const LetterDistribution *ld,
                                  const Rack *rack) {
#if USE_INT128_INTRINSIC
  BitRack bit_rack = 0;
#else
  BitRack bit_rack = {0, 0};
#endif
  for (int ml = 0; ml < ld->size; ml++) {
    const int num_this = rack_get_letter(rack, ml);
#if USE_INT128_INTRINSIC
    bit_rack |= (unsigned __int128)num_this << (ml * BIT_RACK_BITS_PER_LETTER);
#else
    const int shift = ml * BIT_RACK_BITS_PER_LETTER;
    if (shift < 64) {
      bit_rack.low |= (uint64_t)num_this << shift;
    } else {
      bit_rack.high |= (uint64_t)num_this << (shift - 64);
    }
#endif
  }
  return bit_rack;
}

uint8_t bit_rack_get_letter(const BitRack* bit_rack, uint8_t ml) {
  const int shift = ml * BIT_RACK_BITS_PER_LETTER;

#if USE_INT128_INTRINSIC
  return (uint8_t)((*bit_rack >> shift) & ((1 << BIT_RACK_BITS_PER_LETTER) - 1));
#else
  if (shift < 64) {
    return (int)((bit_rack->low >> shift) &
                 ((1 << BIT_RACK_BITS_PER_LETTER) - 1));
  } else {
    return (int)((bit_rack->high >> (shift - 64)) &
                 ((1 << BIT_RACK_BITS_PER_LETTER) - 1));
  }
#endif
}

void bit_rack_add_bit_rack(BitRack *bit_rack, const BitRack *other) {
#if USE_INT128_INTRINSIC
  *bit_rack += *other;
#else
  // No carry is needed: high and low are split at a boundary between letter
  // counts. Letter counts should never overflow, so neither will the internal
  // uint64s.
  bit_rack->low += other->low;
  bit_rack->high += other->high;
#endif
}

// Long division fallback
#if !USE_INT128_INTRINSIC
void bit_rack_div_mod_no_intrinsic(const BitRack *bit_rack, uint32_t divisor,
                                   BitRack *quotient, uint32_t *remainder) {
  const uint64_t highest_32 = bit_rack->high >> 32;
  uint64_t first_quotient_32 = highest_32 / divisor;
  const uint64_t first_remainder_32 = highest_32 % divisor;

  const uint64_t second_dividend =
      (first_remainder_32 << 32) | (bit_rack->high & 0xFFFFFFFF);
  const uint64_t second_quotient = second_dividend / divisor;
  first_quotient_32 += second_quotient >> 32;
  uint64_t second_quotient_32 = second_quotient & 0xFFFFFFFF;
  const uint64_t second_remainder_32 = second_dividend % divisor;

  const uint64_t third_dividend =
      (second_remainder_32 << 32) | (bit_rack->low >> 32);
  const uint64_t third_quotient = third_dividend / divisor;
  second_quotient_32 += third_quotient >> 32;
  uint64_t third_quotient_32 = third_quotient & 0xFFFFFFFF;
  const uint64_t third_remainder_32 = third_dividend % divisor;

  const uint64_t fourth_dividend =
      (third_remainder_32 << 32) | (bit_rack->low & 0xFFFFFFFF);
  const uint64_t fourth_quotient = fourth_dividend / divisor;
  third_quotient_32 += fourth_quotient >> 32;
  const uint32_t fourth_quotient_32 = fourth_quotient & 0xFFFFFFFF;
  const uint64_t fourth_remainder_32 = fourth_dividend % divisor;

  *quotient = (BitRack){(first_quotient_32 << 32) | second_quotient_32,
                        (third_quotient_32 << 32) | fourth_quotient_32};
  *remainder = fourth_remainder_32;
}
#endif

void bit_rack_div_mod(const BitRack *bit_rack, uint32_t divisor,
                      BitRack *quotient, uint32_t *remainder) {
  assert(divisor != 0);

#if USE_INT128_INTRINSIC
  *quotient = *bit_rack / divisor;
  *remainder = *bit_rack % divisor;
#else
  bit_rack_div_mod_no_intrinsic(bit_rack, divisor, quotient, remainder);
#endif
}

uint64_t bit_rack_high_64(const BitRack *bit_rack) {
#if USE_INT128_INTRINSIC
  return (uint64_t)(*bit_rack >> 64);
#else
  return bit_rack->high;
#endif
}

uint64_t bit_rack_low_64(const BitRack *bit_rack) {
#if USE_INT128_INTRINSIC
  return (uint64_t)*bit_rack;
#else
  return bit_rack->low;
#endif
}

#endif