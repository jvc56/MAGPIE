#ifndef RACK_VALUE_TABLE_H
#define RACK_VALUE_TABLE_H

#include "../compat/cmap.h"
#include "../compat/endian_conv.h"
#include "../def/bit_rack_defs.h"
#include "../ent/bit_rack.h"
#include "../ent/equity.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
// It is keyed by BitRack and uses the bucket-chaining layout of the WordMap
// and the rack info table. A rack the census never valued has no entry.
//
// On-disk layout (all integers little-endian):
//   1 byte:  version (RACK_VALUE_TABLE_VERSION)
//   1 byte:  rack_size (matches RACK_SIZE)
//   2 bytes: zero padding
//   4 bytes: num_buckets (a power of 2)
//   4 bytes: num_entries
//   8 bytes: klv_hash, klv_get_hash of the leaves the values belong to
//   (num_buckets + 1) * 4 bytes: bucket_starts
//   num_entries entries, each:
//     16 bytes: the full BitRack
//     4 bytes:  the value (Equity)

enum {
  RACK_VALUE_TABLE_VERSION = 1,
  RACK_VALUE_TABLE_BITRACK_BYTES = 16,
  RACK_VALUE_TABLE_HEADER_BYTES = 20,
  RACK_VALUE_TABLE_MIN_BUCKETS = 16,
};

typedef struct RackValueTableEntry {
  uint8_t bit_rack_bytes[RACK_VALUE_TABLE_BITRACK_BYTES];
  Equity value;
} RackValueTableEntry;

typedef struct RackValueTable {
  char *name;
  uint8_t version;
  uint32_t num_buckets;
  uint32_t num_entries;
  uint64_t klv_hash;
  uint32_t *bucket_starts;
  RackValueTableEntry *entries;
  // When is_mmapped is true, bucket_starts and entries point into the mapped
  // file and are not freed on their own.
  bool is_mmapped;
  CMap map;
} RackValueTable;

// Builds a table of num_racks racks, bit_racks[i] valued values[i]. No rack
// may appear twice.
RackValueTable *rack_value_table_create_from_values(const BitRack *bit_racks,
                                                    const Equity *values,
                                                    uint32_t num_racks,
                                                    uint64_t klv_hash);

// Loads the table named name from data_paths, mapping the file with use_mmap
// (little-endian only) and reading it otherwise.
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

static inline uint32_t
rack_value_table_get_number_of_racks(const RackValueTable *rvt) {
  return rvt->num_entries;
}

// klv_get_hash of the leaves the values were made with: a table read beside
// any other leaves holds values on another scale.
static inline uint64_t
rack_value_table_get_klv_hash(const RackValueTable *rvt) {
  return rvt->klv_hash;
}

static inline BitRack
rack_value_table_entry_read_bit_rack(const RackValueTableEntry *entry) {
#if IS_LITTLE_ENDIAN
  BitRack bit_rack;
  memcpy(&bit_rack, entry->bit_rack_bytes, RACK_VALUE_TABLE_BITRACK_BYTES);
  return bit_rack;
#else
  uint64_t low;
  uint64_t high;
  memcpy(&low, entry->bit_rack_bytes, 8);
  memcpy(&high, entry->bit_rack_bytes + 8, 8);
  low = le64toh(low);
  high = le64toh(high);
  return (BitRack){.low = low, .high = high};
#endif
}

// Sets *value to the full rack's value and returns true, or returns false if
// the table has none for it.
static inline bool rack_value_table_lookup(const RackValueTable *rvt,
                                           const BitRack *bit_rack,
                                           Equity *value) {
  const uint32_t bucket_idx =
      bit_rack_get_bucket_index(bit_rack, rvt->num_buckets);
  const uint32_t start = rvt->bucket_starts[bucket_idx];
  const uint32_t end = rvt->bucket_starts[bucket_idx + 1];
  for (uint32_t entry_idx = start; entry_idx < end; entry_idx++) {
    const RackValueTableEntry *entry = &rvt->entries[entry_idx];
    const BitRack entry_bit_rack = rack_value_table_entry_read_bit_rack(entry);
    if (bit_rack_equals(&entry_bit_rack, bit_rack)) {
      *value = entry->value;
      return true;
    }
  }
  return false;
}

#endif
