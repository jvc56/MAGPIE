#ifndef BIT_RACK_H
#define BIT_RACK_H

#include "../compat/endian_conv.h"
#include "../def/bit_rack_defs.h"
#include "../def/board_defs.h"
#include "dictionary_word.h"
#include "letter_distribution.h"
#include "rack.h"
#include <stdbool.h>
#include <stdint.h>

// These are 128 bit integers representing multisets of tiles, used as keys for
// maps storing words formed by those tiles. They can include both undesignated
// blanks (from players' racks) and designated blanks (from the board).

// Arithmetic addition of two BitRacks performs unions of the multisets.

// Currently finding that the fallback implementation is faster than using
// __int128 intrinsics on some hardware. Disabling intrinsics for now but
// still keeping the code around for possible future use.
//
// #if defined(__SIZEOF_INT128__)
// #define USE_INT128_INTRINSIC 1
// #else
#define USE_INT128_INTRINSIC 0
// #endif

#if USE_INT128_INTRINSIC
typedef unsigned __int128 BitRack;
#else
typedef struct {
#if IS_LITTLE_ENDIAN
  uint64_t low;
  uint64_t high;
#else
  uint64_t high;
  uint64_t low;
#endif
} BitRack;
#endif

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

  // The static analyzer thinks max_letter_count_on_board <=
  // bit_rack_max_letter_count is always true, but it might be false for certain
  // BOARD_DIM
  return ld->size <= BIT_RACK_MAX_ALPHABET_SIZE &&
         // cppcheck-suppress knownConditionTrueFalse
         max_letter_count_on_board <= bit_rack_max_letter_count;
}

static inline bool bit_rack_type_has_expected_size(void) {
  return sizeof(BitRack) == 16;
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
  const MachineLetter *word = dictionary_word_get_word(dictionary_word);
  const uint8_t length = dictionary_word_get_length(dictionary_word);
  for (int i = 0; i < length; i++) {
    const MachineLetter letter = word[i];
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
    const int8_t num_this = rack_get_letter(rack, ml);
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

static inline MachineLetter bit_rack_get_letter(const BitRack *bit_rack,
                                                MachineLetter ml) {
  const int shift = ml * BIT_RACK_BITS_PER_LETTER;

#if USE_INT128_INTRINSIC
  return (MachineLetter)((*bit_rack >> shift) &
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
    const MachineLetter num_ml = bit_rack_get_letter(bit_rack, ml);
    rack->array[ml] = (int8_t)num_ml;
    rack->number_of_letters += (uint16_t)num_ml;
  }
  return rack;
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

static inline void bit_rack_set_letter_count(BitRack *bit_rack,
                                             MachineLetter ml, uint8_t count) {
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

static inline void bit_rack_add_letter(BitRack *bit_rack, MachineLetter ml) {
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

static inline void bit_rack_take_letter(BitRack *bit_rack, MachineLetter ml) {
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
                                           MachineLetter bytes[12]) {
#if USE_INT128_INTRINSIC
  memcpy(bytes, ((MachineLetter *)bit_rack), 12);
#if !IS_LITTLE_ENDIAN
  uint32_t high = bit_rack_get_high_64(bit_rack);
  uint64_t low = bit_rack_get_low_64(bit_rack);
  high = htole32(high);
  low = htole64(low);
  memcpy(bytes, ((MachineLetter *)&low, 8);
  memcpy(bytes + 4, ((MachineLetter *)&high), 4);
#endif
#else
#if IS_LITTLE_ENDIAN
  memcpy(bytes, ((MachineLetter *)&bit_rack->low), 8);
  memcpy(bytes + 8, ((MachineLetter *)&bit_rack->high), 4);
#else
  uint32_t high = bit_rack->high;
  uint64_t low = bit_rack->low;
  high = htole32(high);
  low = htole64(low);
  memcpy(bytes, ((MachineLetter *)low), 8);
  memcpy(bytes + 8, ((MachineLetter *)&high), 4);
#endif
#endif
}

static inline BitRack bit_rack_read_12_bytes(const MachineLetter bytes[12]) {
  BitRack bit_rack = bit_rack_create_empty();
#if USE_INT128_INTRINSIC
  memcpy(((MachineLetter *)&bit_rack), bytes, 12);
#if !IS_LITTLE_ENDIAN
  uint64_t low = bit_rack_get_low_64(&bit_rack);
  uint64_t high = bit_rack_get_high_64(&bit_rack);
  bit_rack_set_high_64(&bit_rack, le64toh(low));
  bit_rack_set_low_64(&bit_rack, le64toh(high));
  bit_rack >>= 32;
#endif
#else
#if IS_LITTLE_ENDIAN
  memcpy(((MachineLetter *)&bit_rack.low), bytes, 8);
  memcpy(((MachineLetter *)&bit_rack.high), bytes + 8, 4);
#else
  uint32_t high;
  memcpy(((MachineLetter *)&high), bytes, 4);
  bit_rack_set_high_64(&bit_rack, le32toh(high));
  memcpy(((MachineLetter *)&bit_rack.low), bytes + 4, 8);
  bit_rack.low = le64toh(bit_rack.low);
#endif
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

static inline bool bit_rack_fits_in_12_bytes(const BitRack *bit_rack) {
  return (bit_rack_get_high_64(bit_rack) & 0xFFFFFFFF00000000ULL) == 0;
}

// Mix 128-bit BitRack to 64-bit hash with good avalanche properties
// This ensures changes in any tile position affect all output bits
static inline uint64_t bit_rack_mix_to_64(const BitRack *bit_rack) {
  uint64_t low = bit_rack_get_low_64(bit_rack);
  uint64_t high = bit_rack_get_high_64(bit_rack);

  // Fold high into low with rotation for better avalanche
  low ^= high;
  low ^= (high << BIT_RACK_HASH_ROTATION_SHIFT) |
         (high >> (64 - BIT_RACK_HASH_ROTATION_SHIFT));

  // MurmurHash3-style mixing for bit diffusion
  low ^= low >> 33;
  low *= BIT_RACK_HASH_MIX_CONSTANT_1;
  low ^= low >> 33;
  low *= BIT_RACK_HASH_MIX_CONSTANT_2;
  low ^= low >> 33;

  return low;
}

// Get bucket index from BitRack using mixing and power-of-2 mask
// num_buckets must be a power of 2
static inline uint32_t bit_rack_get_bucket_index(const BitRack *bit_rack,
                                                  uint32_t num_buckets) {
  const uint64_t hash = bit_rack_mix_to_64(bit_rack);
  return (uint32_t)(hash & (num_buckets - 1));
}
#endif