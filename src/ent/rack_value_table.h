#ifndef RACK_VALUE_TABLE_H
#define RACK_VALUE_TABLE_H

#include "../compat/cmap.h"
#include "../def/bit_rack_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bit_rack.h"
#include "../ent/equity.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A RackValueTable maps full-rack BitRacks to the value of holding that rack:
// what a leave value is to the tiles kept after a play, for the play that
// keeps every tile. The census finds these values on its way to the leaves
// (census_gen.h) and writes them beside the leaves they belong to.
//
// The table is its own file, apart from the leaves and from the rack info
// table: a KLV stops at RACK_SIZE - 1 tiles, and a rack info table belongs to
// a lexicon and its leaves' KLV, while these values exist only for leaves a
// census made.
//
// The file holds no keys. The racks of RACK_SIZE tiles that a letter
// distribution can deal are numbered in the order of their letter counts
// (fewest blanks first, then fewest of the next letter, and so on), and the
// file is the distribution's tile counts followed by one value for each rack
// in that order. A value is the Equity's thousandths of a point as a signed
// 24-bit integer, which reaches 8388.607 points either way; a rack the census
// never valued holds the one 24-bit number below that range.
//
// On-disk layout (all integers little-endian):
//   1 byte:  version (RACK_VALUE_TABLE_VERSION)
//   1 byte:  rack_size (matches RACK_SIZE)
//   1 byte:  num_letters, the size of the letter distribution
//   1 byte:  zero padding
//   4 bytes: num_racks, the number of racks the distribution can deal
//   8 bytes: klv_hash, klv_get_hash of the leaves the values belong to
//   num_letters bytes: the number of tiles of each letter, then zero padding
//                      to a multiple of 4 bytes
//   num_racks * 3 bytes: the values, in the order of the racks

enum {
  RACK_VALUE_TABLE_VERSION = 2,
  RACK_VALUE_TABLE_HEADER_BYTES = 16,
  RACK_VALUE_TABLE_VALUE_BYTES = 3,
  RACK_VALUE_TABLE_MAX_VALUE = (1 << 23) - 1,
  RACK_VALUE_TABLE_MIN_VALUE = -RACK_VALUE_TABLE_MAX_VALUE,
  RACK_VALUE_TABLE_UNVALUED = -(1 << 23),
};

typedef struct RackValueTable {
  char *name;
  uint8_t version;
  uint8_t num_letters;
  uint32_t num_racks;
  uint64_t klv_hash;
  // The number of tiles of each letter, as given; zero past num_letters.
  uint8_t letter_counts[BIT_RACK_MAX_ALPHABET_SIZE];
  // rank_offsets[ml][tiles][count] is the number of racks that come before
  // those with count tiles of letter ml, among the racks that agree on the
  // letters before ml and have tiles tiles left for ml and the letters after
  // it. UINT32_MAX marks a count that no rack can have there.
  uint32_t rank_offsets[BIT_RACK_MAX_ALPHABET_SIZE][(RACK_SIZE) + 1]
                       [(RACK_SIZE) + 1];
  // RACK_VALUE_TABLE_VALUE_BYTES bytes for each rack, as in the file.
  uint8_t *values;
  // When is_mmapped is true, values points into the mapped file and is not
  // freed on its own.
  bool is_mmapped;
  CMap map;
} RackValueTable;

// Builds the table of the distribution with letter_counts[ml] tiles of each of
// its num_letters letters, every rack unvalued. Returns NULL if num_letters is
// not between 1 and BIT_RACK_MAX_ALPHABET_SIZE.
RackValueTable *rack_value_table_create_unvalued(const uint8_t *letter_counts,
                                                 int num_letters,
                                                 uint64_t klv_hash);

// Loads the table named name from data_paths, mapping the file with use_mmap
// and reading it otherwise.
RackValueTable *rack_value_table_create(const char *data_paths,
                                        const char *name, bool use_mmap,
                                        ErrorStack *error_stack);

void rack_value_table_destroy(RackValueTable *rvt);

// Writes the table to the first of data_paths as name.
void rack_value_table_write(const RackValueTable *rvt, const char *data_paths,
                            const char *name, ErrorStack *error_stack);

static inline const char *rack_value_table_get_name(const RackValueTable *rvt) {
  return rvt->name;
}

// The number of racks the table's distribution can deal, valued or not.
static inline uint32_t
rack_value_table_get_number_of_racks(const RackValueTable *rvt) {
  return rvt->num_racks;
}

// The number of those racks that have a value. Reads the whole table.
uint32_t rack_value_table_get_number_of_valued_racks(const RackValueTable *rvt);

// klv_get_hash of the leaves the values were made with: a table read beside
// any other leaves holds values on another scale.
static inline uint64_t
rack_value_table_get_klv_hash(const RackValueTable *rvt) {
  return rvt->klv_hash;
}

// Sets *rack_index to the place of the rack among the racks the table's
// distribution can deal and returns true, or returns false for any other
// BitRack.
static inline bool rack_value_table_get_rack_index(const RackValueTable *rvt,
                                                   const BitRack *bit_rack,
                                                   uint32_t *rack_index) {
  uint32_t index = 0;
  int tiles = (RACK_SIZE);
  for (int ml = 0; ml < BIT_RACK_MAX_ALPHABET_SIZE; ml++) {
    const int count = bit_rack_get_letter(bit_rack, (MachineLetter)ml);
    if (count == 0) {
      continue;
    }
    if (count > tiles) {
      return false;
    }
    const uint32_t offset = rvt->rank_offsets[ml][tiles][count];
    if (offset == UINT32_MAX) {
      return false;
    }
    index += offset;
    tiles -= count;
  }
  if (tiles != 0) {
    return false;
  }
  *rack_index = index;
  return true;
}

// The 24-bit number stored for a rack: its value, or
// RACK_VALUE_TABLE_UNVALUED.
static inline int32_t rack_value_table_read_stored(const RackValueTable *rvt,
                                                   uint32_t rack_index) {
  const uint8_t *bytes =
      rvt->values + (size_t)rack_index * RACK_VALUE_TABLE_VALUE_BYTES;
  const uint32_t low_24_bits = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                               ((uint32_t)bytes[2] << 16);
  // The 24-bit two's complement number, without shifting a negative one.
  return (int32_t)(low_24_bits ^ 0x800000U) - 0x800000;
}

static inline void rack_value_table_write_stored(RackValueTable *rvt,
                                                 uint32_t rack_index,
                                                 int32_t stored) {
  const size_t offset = (size_t)rack_index * RACK_VALUE_TABLE_VALUE_BYTES;
  const uint32_t low_24_bits = (uint32_t)stored & 0xffffffU;
  rvt->values[offset] = (uint8_t)low_24_bits;
  rvt->values[offset + 1] = (uint8_t)(low_24_bits >> 8);
  rvt->values[offset + 2] = (uint8_t)(low_24_bits >> 16);
}

// Values a rack. A value past what the file holds is stored as the nearest
// one it does (RACK_VALUE_TABLE_MIN_VALUE, RACK_VALUE_TABLE_MAX_VALUE).
// Returns false, changing nothing, for a BitRack the table's distribution
// cannot deal or a table that is mapped from its file.
static inline bool rack_value_table_set(RackValueTable *rvt,
                                        const BitRack *bit_rack, Equity value) {
  uint32_t rack_index = 0;
  if (rvt->is_mmapped ||
      !rack_value_table_get_rack_index(rvt, bit_rack, &rack_index)) {
    return false;
  }
  int32_t stored = value;
  if (stored > RACK_VALUE_TABLE_MAX_VALUE) {
    stored = RACK_VALUE_TABLE_MAX_VALUE;
  } else if (stored < RACK_VALUE_TABLE_MIN_VALUE) {
    stored = RACK_VALUE_TABLE_MIN_VALUE;
  }
  rack_value_table_write_stored(rvt, rack_index, stored);
  return true;
}

// Sets *value to the full rack's value and returns true, or returns false if
// the table has none for it.
static inline bool rack_value_table_lookup(const RackValueTable *rvt,
                                           const BitRack *bit_rack,
                                           Equity *value) {
  uint32_t rack_index = 0;
  if (!rack_value_table_get_rack_index(rvt, bit_rack, &rack_index)) {
    return false;
  }
  const int32_t stored = rack_value_table_read_stored(rvt, rack_index);
  if (stored == RACK_VALUE_TABLE_UNVALUED) {
    return false;
  }
  *value = (Equity)stored;
  return true;
}

#endif
