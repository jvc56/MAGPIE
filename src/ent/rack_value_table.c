#include "rack_value_table.h"

#include "../compat/cmap.h"
#include "../compat/endian_conv.h"
#include "../def/rack_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "bit_rack.h"
#include "data_filepaths.h"
#include "equity.h"
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // The bytes a stream is read by, when the table is not mapped.
  RACK_VALUE_TABLE_READ_CHUNK_BYTES = 1 << 20,
};

static void rack_value_table_entry_write_bit_rack(RackValueTableEntry *entry,
                                                  const BitRack *bit_rack) {
  uint64_t low = htole64(bit_rack_get_low_64(bit_rack));
  uint64_t high = htole64(bit_rack_get_high_64(bit_rack));
  memcpy(entry->bit_rack_bytes, &low, 8);
  memcpy(entry->bit_rack_bytes + 8, &high, 8);
}

RackValueTable *rack_value_table_create_from_values(const BitRack *bit_racks,
                                                    const Equity *values,
                                                    uint32_t num_racks,
                                                    uint64_t klv_hash) {
  RackValueTable *rvt = calloc_or_die(1, sizeof(RackValueTable));
  rvt->version = RACK_VALUE_TABLE_VERSION;
  rvt->klv_hash = klv_hash;
  rvt->num_entries = num_racks;
  uint32_t num_buckets = RACK_VALUE_TABLE_MIN_BUCKETS;
  while (num_buckets < num_racks) {
    num_buckets *= 2;
  }
  rvt->num_buckets = num_buckets;
  rvt->bucket_starts = calloc_or_die((size_t)num_buckets + 1, sizeof(uint32_t));
  rvt->entries =
      calloc_or_die(num_racks > 0 ? num_racks : 1, sizeof(RackValueTableEntry));
  // Each bucket's size, one slot along, then its start as the sizes before it.
  for (uint32_t rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    rvt->bucket_starts[bit_rack_get_bucket_index(&bit_racks[rack_idx],
                                                 num_buckets) +
                       1]++;
  }
  for (uint32_t bucket_idx = 0; bucket_idx < num_buckets; bucket_idx++) {
    rvt->bucket_starts[bucket_idx + 1] += rvt->bucket_starts[bucket_idx];
  }
  uint32_t *next_in_bucket = malloc_or_die(sizeof(uint32_t) * num_buckets);
  memcpy(next_in_bucket, rvt->bucket_starts, sizeof(uint32_t) * num_buckets);
  for (uint32_t rack_idx = 0; rack_idx < num_racks; rack_idx++) {
    const uint32_t bucket_idx =
        bit_rack_get_bucket_index(&bit_racks[rack_idx], num_buckets);
    RackValueTableEntry *entry = &rvt->entries[next_in_bucket[bucket_idx]++];
    rack_value_table_entry_write_bit_rack(entry, &bit_racks[rack_idx]);
    entry->value = values[rack_idx];
  }
  free(next_in_bucket);
  return rvt;
}

void rack_value_table_destroy(RackValueTable *rvt) {
  if (!rvt) {
    return;
  }
  free(rvt->name);
  if (rvt->is_mmapped) {
    cmap_close(&rvt->map);
  } else {
    free(rvt->bucket_starts);
    free(rvt->entries);
  }
  free(rvt);
}

static bool rvt_fwrite(const void *ptr, size_t size, size_t nmemb,
                       FILE *stream) {
  return fwrite(ptr, size, nmemb, stream) == nmemb;
}

static bool rvt_write_uint32(uint32_t value, FILE *stream) {
  const uint32_t le = htole32(value);
  return rvt_fwrite(&le, sizeof(le), 1, stream);
}

static bool rvt_write_body(const RackValueTable *rvt, FILE *stream) {
  const uint8_t leading[4] = {rvt->version, (uint8_t)(RACK_SIZE), 0, 0};
  bool written =
      rvt_fwrite(leading, sizeof(uint8_t), sizeof(leading), stream) &&
      rvt_write_uint32(rvt->num_buckets, stream) &&
      rvt_write_uint32(rvt->num_entries, stream) &&
      rvt_write_uint32((uint32_t)rvt->klv_hash, stream) &&
      rvt_write_uint32((uint32_t)(rvt->klv_hash >> 32), stream);
  for (uint32_t bucket_idx = 0; written && bucket_idx <= rvt->num_buckets;
       bucket_idx++) {
    written = rvt_write_uint32(rvt->bucket_starts[bucket_idx], stream);
  }
  for (uint32_t entry_idx = 0; written && entry_idx < rvt->num_entries;
       entry_idx++) {
    const RackValueTableEntry *entry = &rvt->entries[entry_idx];
    written = rvt_fwrite(entry->bit_rack_bytes, sizeof(uint8_t),
                         RACK_VALUE_TABLE_BITRACK_BYTES, stream) &&
              rvt_write_uint32((uint32_t)entry->value, stream);
  }
  return written;
}

void rack_value_table_write(const RackValueTable *rvt, const char *data_paths,
                            const char *name, ErrorStack *error_stack) {
  char *filename = data_filepaths_get_writable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_RACK_VALUE_TABLE, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return;
  }
  // Written beside the final name and renamed into place, so that no reader
  // sees part of a table under its name.
  char *temporary = temporary_sibling(filename);
  FILE *stream = fopen_exclusive_safe(temporary, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(temporary);
    free(filename);
    return;
  }
  errno = 0;
  bool written = rvt_write_body(rvt, stream);
  int error_number = written ? 0 : errno;
  // A full disk often shows only here, when the buffered tail is flushed.
  if (fclose(stream) != 0) {
    if (written) {
      error_number = errno;
    }
    written = false;
  }
  if (written) {
    rename_into_place(temporary, filename, "rack value table", error_stack);
  } else {
    (void)remove(temporary);
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     get_formatted_string(
                         "could not write rack value table %s: %s", filename,
                         error_number != 0 ? strerror(error_number)
                                           : "unknown I/O failure"));
  }
  free(temporary);
  free(filename);
}

static uint32_t rvt_read_uint32(const uint8_t *data) {
  uint32_t value;
  memcpy(&value, data, sizeof(value));
  return le32toh(value);
}

// Reads the table from the size bytes at data. With map_in_place, the bucket
// starts and the entries are left where they lie in data, which must outlive
// the table; otherwise they are copied out of it.
static void rvt_parse(RackValueTable *rvt, const uint8_t *data, size_t size,
                      bool map_in_place, const char *filename,
                      ErrorStack *error_stack) {
  if (size < RACK_VALUE_TABLE_HEADER_BYTES) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("rack value table too small for a header: %s",
                             filename));
    return;
  }
  if (data[0] != RACK_VALUE_TABLE_VERSION) {
    error_stack_push(
        error_stack, ERROR_STATUS_WMP_UNSUPPORTED_VERSION,
        get_formatted_string(
            "detected rack value table version %d but only %d is supported: "
            "%s",
            data[0], RACK_VALUE_TABLE_VERSION, filename));
    return;
  }
  if (data[1] != (RACK_SIZE)) {
    error_stack_push(
        error_stack, ERROR_STATUS_WMP_INCOMPATIBLE_BOARD_DIM,
        get_formatted_string("rack value table rack size %d does not match "
                             "build rack size %d: %s",
                             data[1], (RACK_SIZE), filename));
    return;
  }
  rvt->version = data[0];
  rvt->num_buckets = rvt_read_uint32(data + 4);
  rvt->num_entries = rvt_read_uint32(data + 8);
  rvt->klv_hash = (uint64_t)rvt_read_uint32(data + 12) |
                  ((uint64_t)rvt_read_uint32(data + 16) << 32);

  // Each section is checked against what is left of the file by division, so
  // that corrupt counts cannot overflow a product. A bucket count that is not
  // a power of 2 would send lookups to the wrong bucket.
  const size_t bucket_start_slots = (size_t)rvt->num_buckets + 1;
  const size_t after_header = size - RACK_VALUE_TABLE_HEADER_BYTES;
  bool valid = rvt->num_buckets != 0 &&
               (rvt->num_buckets & (rvt->num_buckets - 1)) == 0 &&
               bucket_start_slots <= after_header / sizeof(uint32_t);
  const size_t entries_offset =
      RACK_VALUE_TABLE_HEADER_BYTES +
      (valid ? bucket_start_slots : 0) * sizeof(uint32_t);
  valid = valid && (size_t)rvt->num_entries <=
                       (size - entries_offset) / sizeof(RackValueTableEntry);
  const uint8_t *bucket_starts_data = data + RACK_VALUE_TABLE_HEADER_BYTES;
  // The starts never go down and end at the entry count, or a lookup would
  // read past the entries.
  uint32_t previous_start = 0;
  for (size_t slot_idx = 0; valid && slot_idx < bucket_start_slots;
       slot_idx++) {
    const uint32_t start =
        rvt_read_uint32(bucket_starts_data + slot_idx * sizeof(uint32_t));
    valid = start >= previous_start && start <= rvt->num_entries;
    previous_start = start;
  }
  valid = valid && previous_start == rvt->num_entries;
  if (!valid) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("rack value table truncated or corrupt: %s",
                             filename));
    return;
  }
  const uint8_t *entries_data = data + entries_offset;
  if (map_in_place) {
    // Mapped files are little-endian hosts' only (rack_value_table_create),
    // and both sections sit at multiples of 4 bytes from the page-aligned
    // start.
    rvt->bucket_starts = (uint32_t *)bucket_starts_data;
    rvt->entries = (RackValueTableEntry *)entries_data;
    return;
  }
  rvt->bucket_starts = malloc_or_die(sizeof(uint32_t) * bucket_start_slots);
  for (size_t slot_idx = 0; slot_idx < bucket_start_slots; slot_idx++) {
    rvt->bucket_starts[slot_idx] =
        rvt_read_uint32(bucket_starts_data + slot_idx * sizeof(uint32_t));
  }
  rvt->entries =
      malloc_or_die(sizeof(RackValueTableEntry) *
                    (rvt->num_entries > 0 ? (size_t)rvt->num_entries : 1));
  for (uint32_t entry_idx = 0; entry_idx < rvt->num_entries; entry_idx++) {
    const uint8_t *entry_data =
        entries_data + (size_t)entry_idx * sizeof(RackValueTableEntry);
    RackValueTableEntry *entry = &rvt->entries[entry_idx];
    memcpy(entry->bit_rack_bytes, entry_data, RACK_VALUE_TABLE_BITRACK_BYTES);
    entry->value =
        (Equity)rvt_read_uint32(entry_data + RACK_VALUE_TABLE_BITRACK_BYTES);
  }
}

static void rvt_load_mapped(RackValueTable *rvt, const char *filename,
                            ErrorStack *error_stack) {
#if !IS_LITTLE_ENDIAN
  (void)rvt;
  error_stack_push(
      error_stack, ERROR_STATUS_RW_READ_ERROR,
      get_formatted_string(
          "mmap mode is not supported on big-endian architectures: %s",
          filename));
#else
  CMap map;
  // Lookups are hash-indexed, so readahead would only waste I/O.
  if (!cmap_open(&map, filename, true)) {
    error_stack_push(
        error_stack,
        errno == ENOENT ? ERROR_STATUS_FILEPATH_FILE_NOT_FOUND
                        : ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("could not map rack value table %s: %s", filename,
                             strerror(errno)));
    return;
  }
  rvt_parse(rvt, (const uint8_t *)map.data, map.size, true, filename,
            error_stack);
  if (!error_stack_is_empty(error_stack)) {
    cmap_close(&map);
    return;
  }
  rvt->is_mmapped = true;
  rvt->map = map;
#endif
}

static void rvt_load_read(RackValueTable *rvt, const char *filename,
                          ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  size_t capacity = RACK_VALUE_TABLE_READ_CHUNK_BYTES;
  size_t size = 0;
  uint8_t *data = malloc_or_die(capacity);
  while (true) {
    const size_t bytes_read = fread(data + size, 1, capacity - size, stream);
    size += bytes_read;
    if (size < capacity) {
      break;
    }
    capacity *= 2;
    data = realloc_or_die(data, capacity);
  }
  const bool read_failed = ferror(stream) != 0;
  fclose_or_die(stream);
  if (read_failed) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("could not read rack value table: %s", filename));
  } else {
    rvt_parse(rvt, data, size, false, filename, error_stack);
  }
  free(data);
}

RackValueTable *rack_value_table_create(const char *data_paths,
                                        const char *name, bool use_mmap,
                                        ErrorStack *error_stack) {
  char *filename = data_filepaths_get_readable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_RACK_VALUE_TABLE, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return NULL;
  }
  RackValueTable *rvt = calloc_or_die(1, sizeof(RackValueTable));
  if (use_mmap) {
    rvt_load_mapped(rvt, filename, error_stack);
  } else {
    rvt_load_read(rvt, filename, error_stack);
  }
  free(filename);
  if (!error_stack_is_empty(error_stack)) {
    // a failed load leaves the table holding nothing
    free(rvt);
    return NULL;
  }
  rvt->name = string_duplicate(name);
  return rvt;
}
