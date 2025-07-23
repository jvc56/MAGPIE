#ifndef ENCODED_RACK_H
#define ENCODED_RACK_H

#include <limits.h>

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"

#include "rack.h"

#include "../util/io_util.h"

#define BITS_TO_REPRESENT(n)                                                   \
  ((n) == 0            ? 1                                                     \
   : (n) <= 1          ? 1                                                     \
   : (n) <= 3          ? 2                                                     \
   : (n) <= 7          ? 3                                                     \
   : (n) <= 15         ? 4                                                     \
   : (n) <= 31         ? 5                                                     \
   : (n) <= 63         ? 6                                                     \
   : (n) <= 127        ? 7                                                     \
   : (n) <= 255        ? 8                                                     \
   : (n) <= 511        ? 9                                                     \
   : (n) <= 1023       ? 10                                                    \
   : (n) <= 2047       ? 11                                                    \
   : (n) <= 4095       ? 12                                                    \
   : (n) <= 8191       ? 13                                                    \
   : (n) <= 16383      ? 14                                                    \
   : (n) <= 32767      ? 15                                                    \
   : (n) <= 65535      ? 16                                                    \
   : (n) <= 131071     ? 17                                                    \
   : (n) <= 262143     ? 18                                                    \
   : (n) <= 524287     ? 19                                                    \
   : (n) <= 1048575    ? 20                                                    \
   : (n) <= 2097151    ? 21                                                    \
   : (n) <= 4194303    ? 22                                                    \
   : (n) <= 8388607    ? 23                                                    \
   : (n) <= 16777215   ? 24                                                    \
   : (n) <= 33554431   ? 25                                                    \
   : (n) <= 67108863   ? 26                                                    \
   : (n) <= 134217727  ? 27                                                    \
   : (n) <= 268435455  ? 28                                                    \
   : (n) <= 536870911  ? 29                                                    \
   : (n) <= 1073741823 ? 30                                                    \
                       : 31)

#define ENCODED_RACK_UNIT_TYPE uint64_t

// Use -1 for bits per ml since the ml bits just represent the tiles and aren't
// specifying a count of anything.
#define BITS_PER_ML (uint8_t)BITS_TO_REPRESENT(MAX_ALPHABET_SIZE - 1)
// Do not use any adjustments for RACK_SIZE since this represents the actual
// count. For example, a rack size of 8 has 9 distinct counts (0, 1, 2, ..., 8).
// We could possibly do an increment on the read to convert the actual bit
// values from [0, 7] to [1, 8] since there will never be zero of a tile in the
// encoded rack, but we'll leave that for later.
#define BITS_PER_COUNT (uint8_t)BITS_TO_REPRESENT(RACK_SIZE)
#define BITS_PER_ML_AND_COUNT (BITS_PER_ML + BITS_PER_COUNT)
#define BITS_PER_UNIT (uint8_t)(sizeof(ENCODED_RACK_UNIT_TYPE) * CHAR_BIT)
#define BITS_PER_RACK (uint8_t)(BITS_PER_ML_AND_COUNT * RACK_SIZE)
#define ENCODED_RACK_UNITS                                                     \
  (uint8_t)((BITS_PER_RACK + BITS_PER_UNIT - 1) / BITS_PER_UNIT)

typedef struct EncodedRack {
  ENCODED_RACK_UNIT_TYPE array[ENCODED_RACK_UNITS];
} EncodedRack;

static inline ENCODED_RACK_UNIT_TYPE
encoded_rack_shift_bit(uint32_t bit_index) {
  return (ENCODED_RACK_UNIT_TYPE)1 << (bit_index % BITS_PER_UNIT);
}

static inline void encoded_rack_set_bit(EncodedRack *encoded_rack,
                                        uint32_t bit_index) {
  encoded_rack->array[bit_index / BITS_PER_UNIT] |=
      encoded_rack_shift_bit(bit_index);
}

static inline bool encoded_rack_get_bit(const EncodedRack *encoded_rack,
                                        uint32_t bit_index) {
  return (encoded_rack->array[bit_index / BITS_PER_UNIT] &
          encoded_rack_shift_bit(bit_index)) != 0;
}

static inline void encoded_rack_set_ml(EncodedRack *encoded_rack,
                                       MachineLetter ml, uint32_t *bit_index) {
  uint32_t start_bit_index = *bit_index;
  while (ml) {
    if (ml & 1) {
      encoded_rack_set_bit(encoded_rack, *bit_index);
    }
    ml >>= 1;
    (*bit_index)++;
  }
  *bit_index += BITS_PER_ML - (*bit_index - start_bit_index);
}

static inline MachineLetter encoded_rack_get_ml(const EncodedRack *encoded_rack,
                                                uint32_t *bit_index) {
  MachineLetter ml = 0;
  for (int i = 0; i < BITS_PER_ML; i++) {
    if (encoded_rack_get_bit(encoded_rack, *bit_index)) {
      ml |= 1 << i;
    }
    (*bit_index)++;
  }
  return ml;
}

static inline void encoded_rack_set_count(EncodedRack *encoded_rack, int count,
                                          uint32_t *bit_index) {
  uint32_t start_bit_index = *bit_index;
  while (count) {
    if (count & 1) {
      encoded_rack_set_bit(encoded_rack, *bit_index);
    }
    count >>= 1;
    (*bit_index)++;
  }
  *bit_index += BITS_PER_COUNT - (*bit_index - start_bit_index);
}

static inline uint32_t encoded_rack_get_count(const EncodedRack *encoded_rack,
                                              uint32_t *bit_index) {
  int count = 0;
  for (uint8_t i = 0; i < BITS_PER_COUNT; i++) {
    if (encoded_rack_get_bit(encoded_rack, *bit_index)) {
      count |= 1 << i;
    }
    (*bit_index)++;
  }
  return count;
}

static inline bool rack_encode_at_end(uint32_t bit_index) {
  return (ENCODED_RACK_UNIT_TYPE)bit_index >= BITS_PER_RACK;
}

static inline void rack_encode(const Rack *rack, EncodedRack *encoded_rack) {
  for (int i = 0; i < (int)ENCODED_RACK_UNITS; i++) {
    encoded_rack->array[i] = 0;
  }
  uint32_t bit_index = 0;
  for (int ml = 0; ml < rack->dist_size; ml++) {
    const int8_t number_of_ml = rack_get_letter(rack, ml);
    if (number_of_ml > 0) {
      encoded_rack_set_ml(encoded_rack, ml, &bit_index);
      encoded_rack_set_count(encoded_rack, number_of_ml, &bit_index);
    }
  }
  if (!rack_encode_at_end(bit_index)) {
    encoded_rack_set_ml(encoded_rack, MAX_ALPHABET_SIZE, &bit_index);
  }
}

static inline void rack_decode(const EncodedRack *encoded_rack, Rack *rack) {
  rack_reset(rack);
  uint32_t bit_index = 0;
  while (true) {
    MachineLetter curr_ml = encoded_rack_get_ml(encoded_rack, &bit_index);
    if (curr_ml == MAX_ALPHABET_SIZE) {
      break;
    }
    const uint32_t number_of_ml =
        encoded_rack_get_count(encoded_rack, &bit_index);
    rack_add_letters(rack, curr_ml, number_of_ml);
    if (rack_encode_at_end(bit_index)) {
      break;
    }
  }
}

#endif