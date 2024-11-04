#ifndef BIT_RACK_H
#define BIT_RACK_H

#include "../compat/endian_conv.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "../def/bit_rack_defs.h"
#include "../def/board_defs.h"
#include "dictionary_word.h"
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
  uint64_t low;
  uint64_t high;
} BitRack;
#endif

typedef struct BitRackPowerSet {
  BitRack subracks[1 << RACK_SIZE];
  uint8_t count_by_size[RACK_SIZE + 1];
} BitRackPowerSet;

static inline bool
bit_rack_is_compatible_with_ld(const LetterDistribution *ld) {
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

static inline bool bit_rack_type_has_expected_size(void) {
  return sizeof(BitRack) == 16;
}

static inline bool bit_rack_is_compatible_with_endianness(void) {
#if defined(__APPLE__)
  return OSHostByteOrder() == OSLittleEndian;
#else
  return __BYTE_ORDER == __LITTLE_ENDIAN;
#endif
}

static inline BitRack bit_rack_create_empty(void) {
#if USE_INT128_INTRINSIC
  return 0;
#else
  return (BitRack){0, 0};
#endif
}

static inline BitRack
bit_rack_create_from_dictionary_word(const DictionaryWord *dictionary_word) {
  BitRack bit_rack = bit_rack_create_empty();
  const uint8_t *word = dictionary_word_get_word(dictionary_word);
  const uint8_t length = dictionary_word_get_length(dictionary_word);
  for (int i = 0; i < length; i++) {
    const uint8_t letter = word[i];
    const int shift = letter * BIT_RACK_BITS_PER_LETTER;
#if USE_INT128_INTRINSIC
    bit_rack += (unsigned __int128)1 << shift;
#else
    if (shift < 64) {
      bit_rack.low += 1ULL << shift;
    } else {
      bit_rack.high += 1ULL << (shift - 64);
    }
#endif
  }
  return bit_rack;
}

static inline BitRack bit_rack_create_from_rack(const LetterDistribution *ld,
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

static inline uint8_t bit_rack_get_letter(const BitRack *bit_rack, uint8_t ml) {
  const int shift = ml * BIT_RACK_BITS_PER_LETTER;

#if USE_INT128_INTRINSIC
  return (uint8_t)((*bit_rack >> shift) &
                   ((1 << BIT_RACK_BITS_PER_LETTER) - 1));
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

static inline void bit_rack_add_bit_rack(BitRack *bit_rack,
                                         const BitRack *other) {
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
static inline void bit_rack_div_mod_no_intrinsic(const BitRack *bit_rack,
                                                 uint32_t divisor,
                                                 BitRack *quotient,
                                                 uint32_t *remainder) {
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

  *quotient = (BitRack){(third_quotient_32 << 32) | fourth_quotient_32,
                        (first_quotient_32 << 32) | second_quotient_32};
  *remainder = fourth_remainder_32;
}
#endif

static inline void bit_rack_div_mod(const BitRack *bit_rack, uint32_t divisor,
                                    BitRack *quotient, uint32_t *remainder) {
  assert(divisor != 0);

#if USE_INT128_INTRINSIC
  *quotient = *bit_rack / divisor;
  *remainder = *bit_rack % divisor;
#else
  bit_rack_div_mod_no_intrinsic(bit_rack, divisor, quotient, remainder);
#endif
}

static inline uint64_t bit_rack_get_high_64(const BitRack *bit_rack) {
#if USE_INT128_INTRINSIC
  return (uint64_t)(*bit_rack >> 64);
#else
  return bit_rack->high;
#endif
}

static inline uint64_t bit_rack_get_low_64(const BitRack *bit_rack) {
#if USE_INT128_INTRINSIC
  return (uint64_t)*bit_rack;
#else
  return bit_rack->low;
#endif
}

static inline bool bit_rack_equals(const BitRack *a, const BitRack *b) {
#if USE_INT128_INTRINSIC
  return *a == *b;
#else
  return (a->high == b->high) && (a->low == b->low);
#endif
}

static inline Rack *bit_rack_to_rack(const BitRack *bit_rack) {
  Rack *rack = rack_create(BIT_RACK_MAX_ALPHABET_SIZE);
  for (int ml = 0; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    rack->array[ml] = bit_rack_get_letter(bit_rack, ml);
    rack->number_of_letters += rack->array[ml];
  }
  return rack;
}

static inline void bit_rack_add_uint32(BitRack *bit_rack, uint32_t addend) {
#if USE_INT128_INTRINSIC
  *bit_rack += addend;
#else
  const uint64_t low_sum = bit_rack->low + addend;
  bool overflow = low_sum < bit_rack->low;
  bit_rack->low = low_sum;
  bit_rack->high += overflow;
#endif
}

static inline BitRack bit_rack_mul(const BitRack *bit_rack,
                                   uint32_t multiplier) {
#if USE_INT128_INTRINSIC
  return *bit_rack * multiplier;
#else
  const uint64_t lowest_32 = bit_rack->low & 0xFFFFFFFF;
  const uint64_t second_32 = bit_rack->low >> 32;
  const uint64_t third_32 = bit_rack->high; // fits in 32 bits, no need to mask

  const uint64_t low_product = lowest_32 * multiplier;
  uint64_t carry = low_product >> 32;
  const uint64_t lowest_result = low_product & 0xFFFFFFFF;

  const uint64_t second_product = second_32 * multiplier + carry;
  carry = second_product >> 32;
  const uint64_t second_result = second_product & 0xFFFFFFFF;

  const uint64_t high_result = third_32 * multiplier + carry;
  const uint64_t low_result = (second_result << 32) | lowest_result;
  return (BitRack){low_result, high_result};
#endif
}

static inline void bit_rack_set_letter_count(BitRack *bit_rack, uint8_t ml,
                                             uint8_t count) {
  const int shift = ml * BIT_RACK_BITS_PER_LETTER;
#if USE_INT128_INTRINSIC
  *bit_rack &=
      ~((unsigned __int128)((1 << BIT_RACK_BITS_PER_LETTER) - 1) << shift);
  *bit_rack |= (unsigned __int128)count << shift;
#else
  uint64_t mask = ((uint64_t)1 << BIT_RACK_BITS_PER_LETTER) - 1;
  if (shift < 64) {
    bit_rack->low &= ~(mask << shift);
    bit_rack->low |= ((uint64_t)count) << shift;
  } else {
    const int adjusted_shift = shift - 64;
    bit_rack->high &= ~(mask << adjusted_shift);
    bit_rack->high |= ((uint64_t)count) << adjusted_shift;
  }
#endif
}

static inline BitRack largest_bit_rack_for_ld(const LetterDistribution *ld) {
  BitRack bit_rack = bit_rack_create_empty();
  int letters_to_add = BOARD_DIM;
  for (int ml = ld->size - 1; ml >= 1; ml--) {
    int letter_count = ld->distribution[ml];
    if (ml == ld->size - 1) {
      letter_count += ld->distribution[BLANK_MACHINE_LETTER];
    }
    if (letters_to_add >= letter_count) {
      bit_rack_set_letter_count(&bit_rack, ml, letter_count);
      letters_to_add -= letter_count;
    } else {
      bit_rack_set_letter_count(&bit_rack, ml, letters_to_add);
      return bit_rack;
    }
  }
  return bit_rack;
}

static inline void bit_rack_add_letter(BitRack *bit_rack, uint8_t ml) {
  const int shift = ml * BIT_RACK_BITS_PER_LETTER;
#if USE_INT128_INTRINSIC
  *bit_rack += (unsigned __int128)1 << shift;
#else
  if (shift < 64) {
    bit_rack->low += (uint64_t)1 << shift;
  } else {
    bit_rack->high += (uint64_t)1 << (shift - 64);
  }
#endif
}

static inline void bit_rack_take_letter(BitRack *bit_rack, uint8_t ml) {
  const int shift = ml * BIT_RACK_BITS_PER_LETTER;
#if USE_INT128_INTRINSIC
  *bit_rack -= (unsigned __int128)1 << shift;
#else
  if (shift < 64) {
    bit_rack->low -= (uint64_t)1 << shift;
  } else {
    bit_rack->high -= (uint64_t)1 << (shift - 64);
  }
#endif
}

static inline void bit_rack_write_12_bytes(const BitRack *bit_rack,
                                           uint8_t bytes[12]) {
  // assumes little-endian
#if USE_INT128_INTRINSIC
  memory_copy(bytes, ((uint8_t *)bit_rack), 12);
#else
  memory_copy(bytes, ((uint8_t *)&bit_rack->low), 8);
  memory_copy(bytes + 8, ((uint8_t *)&bit_rack->high), 4);
#endif
}

static inline BitRack bit_rack_read_12_bytes(const uint8_t bytes[12]) {
  BitRack bit_rack = bit_rack_create_empty();
  // assumes little-endian
#if USE_INT128_INTRINSIC
  memory_copy(((uint8_t *)&bit_rack), bytes, 12);
#else
  memory_copy(((uint8_t *)&bit_rack.low), bytes, 8);
  memory_copy(((uint8_t *)&bit_rack.high), bytes + 8, 4);
#endif
  return bit_rack;
}

static inline int bit_rack_num_letters(const BitRack *bit_rack) {
  int num_letters = 0;
  for (int ml = 0; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    num_letters += bit_rack_get_letter(bit_rack, ml);
  }
  return num_letters;
}

static const uint8_t bit_rack_combination_offsets[] = {
    BIT_RACK_COMBINATION_OFFSETS};

static inline uint8_t bit_rack_get_combination_offset(int size) {
  return bit_rack_combination_offsets[size];
}

static inline void fill_bit_rack_power_set(const BitRack *full,
                                           BitRack *current, uint8_t next_ml,
                                           int count, BitRackPowerSet *set) {
  int max_num_this = 0;
  for (; next_ml < BIT_RACK_MAX_ALPHABET_SIZE; next_ml++) {
    max_num_this = bit_rack_get_letter(full, next_ml);
    if (max_num_this > 0) {
      break;
    }
  }
  if (next_ml >= BIT_RACK_MAX_ALPHABET_SIZE) {
    const int insert_index =
        bit_rack_get_combination_offset(count) + set->count_by_size[count];
    set->subracks[insert_index] = *current;
    set->count_by_size[count]++;
    return;
  }
  for (int i = 0; i <= max_num_this; i++) {
    fill_bit_rack_power_set(full, current, next_ml + 1, count + i, set);
    bit_rack_add_letter(current, next_ml);
  }
  bit_rack_set_letter_count(current, next_ml, 0);
}

static inline void bit_rack_power_set_init(BitRackPowerSet *set,
                                           const BitRack *full) {
  memset(set->count_by_size, 0, sizeof(set->count_by_size));
  BitRack empty = bit_rack_create_empty();
  fill_bit_rack_power_set(full, &empty, BLANK_MACHINE_LETTER, 0, set);
}

static inline bool bit_rack_fits_in_12_bytes(const BitRack *bit_rack) {
  return (bit_rack_get_high_64(bit_rack) & 0xFFFFFFFF00000000ULL) == 0;
}
#endif